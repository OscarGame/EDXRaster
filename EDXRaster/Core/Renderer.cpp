
#include "Renderer.h"
#include "FrameBuffer.h"
#include "Rasterizer.h"
#include "Scene.h"
#include "Clipper.h"
#include "../Utils/Mesh.h"
#include "../Utils/InputBuffer.h"
#include "Math/Matrix.h"
#include "Windows/Bitmap.h"
#include "Windows/Application.h"

#include <ppl.h>
using namespace concurrency;

namespace EDX
{
	namespace RasterRenderer
	{
		RenderStates* RenderStates::mpInstance = nullptr;

		void Renderer::Initialize(uint iScreenWidth, uint iScreenHeight)
		{
			RenderStates::Instance()->DefaultSettings();

			mTileDim.x = (iScreenWidth + Tile::SIZE - 1) >> Tile::SIZE_LOG_2;
			mTileDim.y = (iScreenHeight + Tile::SIZE - 1) >> Tile::SIZE_LOG_2;

			if (!mpFrameBuffer)
			{
				mpFrameBuffer = MakeUnique<FrameBuffer>();
			}
			mpFrameBuffer->Init(iScreenWidth, iScreenHeight, mTileDim, RenderStates::Instance()->MultiSampleLevel);

			if (!mpScene)
			{
				mpScene = MakeUnique<Scene>();
			}

			mpVertexShader = MakeUnique<DefaultVertexShader>();
			mpPixelShader = MakeUnique<LambertianAlbedoPixelShader>();

			int tId = 0;
			for (auto i = 0; i < iScreenHeight; i += Tile::SIZE)
			{
				for (auto j = 0; j < iScreenWidth; j += Tile::SIZE)
				{
					auto maxX = Math::Min(j + Tile::SIZE, iScreenWidth);
					auto maxY = Math::Min(i + Tile::SIZE, iScreenHeight);

					mTiles.Add(Tile(Vector2i(j, i), Vector2i(maxX, maxY), tId++));
				}
			}

			mNumCores = GetNumberOfCores();
			mWriteFrames = false;

			mpDistributedProjVertexBuf = new Array<ProjectedVertex>[mNumCores];
			mpRasterTriangleBuf = new Array<RasterTriangle>[mNumCores];

			mpRasterizer = MakeUnique<Rasterizer>(mpFrameBuffer.Get(), mpDistributedProjVertexBuf);
		}

		void Renderer::Resize(uint iScreenWidth, uint iScreenHeight)
		{
			mTileDim.x = (iScreenWidth + Tile::SIZE - 1) >> Tile::SIZE_LOG_2;
			mTileDim.y = (iScreenHeight + Tile::SIZE - 1) >> Tile::SIZE_LOG_2;

			mpFrameBuffer->Resize(iScreenWidth, iScreenHeight, mTileDim, RenderStates::Instance()->MultiSampleLevel);

			mTiles.Clear();
			int tId = 0;
			for (auto i = 0; i < iScreenHeight; i += Tile::SIZE)
			{
				for (auto j = 0; j < iScreenWidth; j += Tile::SIZE)
				{
					auto maxX = Math::Min(j + Tile::SIZE, iScreenWidth);
					auto maxY = Math::Min(i + Tile::SIZE, iScreenHeight);

					mTiles.Add(Tile(Vector2i(j, i), Vector2i(maxX, maxY), tId++));
				}
			}
		}

		void Renderer::SetTransform(const Matrix& mModelView, const Matrix& mProj, const Matrix& mToRaster)
		{
			RenderStates::Instance()->ModelViewMatrix = mModelView;
			RenderStates::Instance()->ModelViewInvMatrix = Matrix::Inverse(mModelView);
			RenderStates::Instance()->ProjMatrix = mProj;
			RenderStates::Instance()->ModelViewProjMatrix = mProj * mModelView;
			RenderStates::Instance()->RasterMatrix = mToRaster;
		}

		void Renderer::SetMSAAMode(const int sampleCountLog2)
		{
			RenderStates::Instance()->MultiSampleLevel = sampleCountLog2;
			Resize(mpFrameBuffer->GetWidth(), mpFrameBuffer->GetHeight());
		}

		void Renderer::RenderMesh(const Mesh& mesh)
		{
			// Clear framebuffer
			mpFrameBuffer->Clear();

			// Set texture index
			RenderStates::Instance()->TextureSlots = &mesh.GetTextures();

			VertexProcessing(mesh.GetVertexBuffer());
			Clipping(mesh.GetIndexBuffer(), mesh.GetTextureIds());
			TiledRasterization();
			FragmentProcessing();
			UpdateFrameBuffer();

			if (mWriteFrames)
				WriteFrameToFile();

			RenderStates::Instance()->FrameCount++;
		}

		void Renderer::VertexProcessing(const IVertexBuffer* pVertexBuf)
		{
			mProjectedVertexBuf.Resize(pVertexBuf->GetVertexCount());
			parallel_for(0, (int)pVertexBuf->GetVertexCount(), [&](int i)
			{
				mpVertexShader->Execute(pVertexBuf->GetPosition(i), pVertexBuf->GetNormal(i), pVertexBuf->GetTexCoord(i), &mProjectedVertexBuf[i]);
			});
		}

		void Renderer::Clipping(IndexBuffer* pIndexBuf, const Array<uint>& texIdBuf)
		{
			parallel_for(0, mNumCores, [&](int coreId)
			{
				mpDistributedProjVertexBuf[coreId].Clear();
				mpRasterTriangleBuf[coreId].Clear();
			});

			Clipper::Clip(mProjectedVertexBuf, pIndexBuf, texIdBuf, mpDistributedProjVertexBuf, mpRasterTriangleBuf, mNumCores);

			parallel_for(0, mNumCores, [&](int coreId)
			{
				for (auto i = 0; i < mpDistributedProjVertexBuf[coreId].Size(); i++)
				{
					ProjectedVertex& vertex = mpDistributedProjVertexBuf[coreId][i];
					vertex.invW = 1.0f / vertex.projectedPos.w;
					vertex.projectedPos.z *= vertex.invW;
				}
			});
		}

		void Renderer::TiledRasterization()
		{
			// Binning triangles
			parallel_for(0, (int)mTiles.Size(), [&](int i)
			{
				for (auto c = 0; c < mNumCores; c++)
					mTiles[i].triangleRefs[c].Clear();

				mTiles[i].fragmentBuf.Clear();
			});

			const int Shift = Tile::SIZE_LOG_2 + 4;
			parallel_for(0, mNumCores, [&](int coreId)
			{
				for (auto i = 0; i < mpRasterTriangleBuf[coreId].Size(); i++)
				{
					const RasterTriangle& tri = mpRasterTriangleBuf[coreId][i];

					int minX = Math::Max(0, Math::Min(tri.v0.x, Math::Min(tri.v1.x, tri.v2.x)) >> Shift);
					int maxX = Math::Min(mTileDim.x - 1, Math::Max(tri.v0.x, Math::Max(tri.v1.x, tri.v2.x)) >> Shift);
					int minY = Math::Max(0, Math::Min(tri.v0.y, Math::Min(tri.v1.y, tri.v2.y)) >> Shift);
					int maxY = Math::Min(mTileDim.y - 1, Math::Max(tri.v0.y, Math::Max(tri.v1.y, tri.v2.y)) >> Shift);

					if (maxX - minX < 2 && maxY - minY < 2)
					{
						for (auto y = minY; y <= maxY; y++)
						{
							for (auto x = minX; x <= maxX; x++)
								mTiles[y * mTileDim.x + x].triangleRefs[coreId].Add(Tile::TriangleRef(i));
						}
					}
					else
					{
						for (auto y = minY; y <= maxY; y++)
						{
							for (auto x = minX; x <= maxX; x++)
							{
								Vector2i pixelBase = Vector2i(x, y);

								const Vector2i rejCornerOffset0 = Vector2i(tri.rejectCorner0 % 2, tri.rejectCorner0 / 2);
								const Vector2i rejCornerOffset1 = Vector2i(tri.rejectCorner1 % 2, tri.rejectCorner1 / 2);
								const Vector2i rejCornerOffset2 = Vector2i(tri.rejectCorner2 % 2, tri.rejectCorner2 / 2);

								const Vector2i rejCorner0 = Vector2i(
									(pixelBase.x + rejCornerOffset0.x) << Shift,
									(pixelBase.y + rejCornerOffset0.y) << Shift);
								const Vector2i rejCorner1 = Vector2i(
									(pixelBase.x + rejCornerOffset1.x) << Shift,
									(pixelBase.y + rejCornerOffset1.y) << Shift);
								const Vector2i rejCorner2 = Vector2i(
									(pixelBase.x + rejCornerOffset2.x) << Shift,
									(pixelBase.y + rejCornerOffset2.y) << Shift);

								if (tri.EdgeFunc0(rejCorner0) < 0 || tri.EdgeFunc1(rejCorner1) < 0 || tri.EdgeFunc2(rejCorner2) < 0)
									continue;
								
								const Vector2i acptCornerOffset0 = Vector2i(tri.acceptCorner0 % 2, tri.acceptCorner0 / 2);
								const Vector2i acptCornerOffset1 = Vector2i(tri.acceptCorner1 % 2, tri.acceptCorner1 / 2);
								const Vector2i acptCornerOffset2 = Vector2i(tri.acceptCorner2 % 2, tri.acceptCorner2 / 2);

								const Vector2i acptCorner0 = Vector2i(
									(pixelBase.x + acptCornerOffset0.x) << Shift,
									(pixelBase.y + acptCornerOffset0.y) << Shift);
								const Vector2i acptCorner1 = Vector2i(
									(pixelBase.x + acptCornerOffset1.x) << Shift,
									(pixelBase.y + acptCornerOffset1.y) << Shift);
								const Vector2i acptCorner2 = Vector2i(
									(pixelBase.x + acptCornerOffset2.x) << Shift,
									(pixelBase.y + acptCornerOffset2.y) << Shift);

								mTiles[y * mTileDim.x + x].triangleRefs[coreId].Add(Tile::TriangleRef(i,
									tri.EdgeFunc0(acptCorner0) >= 0,
									tri.EdgeFunc1(acptCorner1) >= 0,
									tri.EdgeFunc2(acptCorner2) >= 0,
									true));
							}
						}
					}
				}
			});


			//for (auto i = 0; i < mTiles.Size(); i++)
			parallel_for(0, (int)mTiles.Size(), [&](int i)
			{
				RasterizeTile(mTiles[i]);
			});

			mFragmentBuf.Clear();
			mTiledShadingResultBuf.Resize(mTiles.Size());
			for (auto i = 0; i < mTiles.Size(); i++)
			{
				mTiledShadingResultBuf[i].Resize(mTiles[i].fragmentBuf.Size());
				if (mTiles[i].fragmentBuf.Size() > 0)
					mFragmentBuf.Insert(mTiles[i].fragmentBuf.Data(), mTiles[i].fragmentBuf.Size(), mFragmentBuf.Size());
			}
		}

		void Renderer::RasterizeTile(Tile& tile)
		{
			for (auto coreId = 0; coreId < mNumCores; coreId++)
			{
				for (auto j = 0; j < tile.triangleRefs[coreId].Size(); j++)
				{
					const Tile::TriangleRef& triRef = tile.triangleRefs[coreId][j];
					RasterTriangle& tri = mpRasterTriangleBuf[coreId][triRef.triId];

					if (triRef.trivialAccept)
					{
						mpRasterizer->TrivialAcceptTriangle(tile, tile.minCoord, tile.maxCoord, tri);
						continue;
					}

					if (RenderStates::Instance()->HierarchicalRasterize && triRef.big)
						mpRasterizer->CoarseRasterize(tile, triRef, Tile::SIZE, tile.minCoord, tile.maxCoord, tri);
					else
						mpRasterizer->FineRasterize(tile, triRef, Tile::SIZE, tile.minCoord, tile.maxCoord, tri);
				}
			}

		}

		void Renderer::FragmentProcessing()
		{
			//for (auto i = 0; i < mFragmentBuf.Size(); i++)
			parallel_for(0, (int)mFragmentBuf.Size(), [&](int i)
			{
				Fragment& frag = mFragmentBuf[i];

				const ProjectedVertex& v0 = mpDistributedProjVertexBuf[frag.coreId][frag.vId0];
				const ProjectedVertex& v1 = mpDistributedProjVertexBuf[frag.coreId][frag.vId1];
				const ProjectedVertex& v2 = mpDistributedProjVertexBuf[frag.coreId][frag.vId2];

				Vec3f_SSE position;
				Vec3f_SSE normal;
				Vec2f_SSE texCoord;
				frag.Interpolate(v0, v1, v2, frag.lambda0, frag.lambda1, position, normal, texCoord);

				Vec3f_SSE shadingResults = mpPixelShader->Shade(frag,
					Matrix::TransformPoint(Vector3::ZERO, RenderStates::Instance()->GetModelViewInvMatrix()),
					Vector3(1, 1, -1),
					position,
					normal,
					texCoord);

				Color4b colorByte[4];
				colorByte[0].FromFloats(shadingResults.x[0], shadingResults.y[0], shadingResults.z[0]);
				colorByte[1].FromFloats(shadingResults.x[1], shadingResults.y[1], shadingResults.z[1]);
				colorByte[2].FromFloats(shadingResults.x[2], shadingResults.y[2], shadingResults.z[2]);
				colorByte[3].FromFloats(shadingResults.x[3], shadingResults.y[3], shadingResults.z[3]);

				mTiledShadingResultBuf[frag.tileId][frag.intraTileIdx] = _mm_loadu_si128((__m128i*)&colorByte);
			});
		}

		void Renderer::UpdateFrameBuffer()
		{
			parallel_for(0, (int)mTiledShadingResultBuf.Size(), [&](int i)
			{
				for (auto j = 0; j < mTiles[i].fragmentBuf.Size(); j++)
				{
					const Fragment& frag = mTiles[i].fragmentBuf[j];
					for (auto sId = 0; sId < mpFrameBuffer->GetSampleCount(); sId++)
					{
						int maskShift = sId << 2;

						const IntSSE& quadResults = mTiledShadingResultBuf[i][j];
						if (frag.coverageMask.GetBit(maskShift) != 0)
						{
							mpFrameBuffer->SetPixel(Color4b(quadResults.m128.m128i_u8[0],
								quadResults.m128.m128i_u8[1],
								quadResults.m128.m128i_u8[2]),
								frag.x, frag.y, sId);
						}
						if (frag.coverageMask.GetBit(maskShift + 1) != 0)
						{
							mpFrameBuffer->SetPixel(Color4b(quadResults.m128.m128i_u8[4],
								quadResults.m128.m128i_u8[5],
								quadResults.m128.m128i_u8[6]),
								frag.x + 1, frag.y, sId);
						}
						if (frag.coverageMask.GetBit(maskShift + 2) != 0)
						{
							mpFrameBuffer->SetPixel(Color4b(quadResults.m128.m128i_u8[8],
								quadResults.m128.m128i_u8[9],
								quadResults.m128.m128i_u8[10]),
								frag.x, frag.y + 1, sId);
						}
						if (frag.coverageMask.GetBit(maskShift + 3) != 0)
						{
							mpFrameBuffer->SetPixel(Color4b(quadResults.m128.m128i_u8[12],
								quadResults.m128.m128i_u8[13],
								quadResults.m128.m128i_u8[14]),
								frag.x + 1, frag.y + 1, sId);
						}
					}
				}
			});

			mpFrameBuffer->Resolve();
		}

		void Renderer::WriteFrameToFile() const
		{
			char fileName[MAX_PATH];
			sprintf_s(fileName, MAX_PATH, "%s/Frames/Frame%05i.bmp", Application::GetBaseDirectory(), RenderStates::Instance()->FrameCount);

			Bitmap::SaveBitmapFile(fileName, GetBackBuffer(), mpFrameBuffer->GetWidth(), mpFrameBuffer->GetHeight());
		}

		const _byte* Renderer::GetBackBuffer() const
		{
			return mpFrameBuffer->GetColorBuffer();
		}

		Renderer::~Renderer()
		{
			Memory::SafeDeleteArray(mpDistributedProjVertexBuf);
			Memory::SafeDeleteArray(mpRasterTriangleBuf);

			RenderStates::DeleteInstance();
		}
	}
}