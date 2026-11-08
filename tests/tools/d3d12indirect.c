/* d3d12indirect: the two things Chrome's WebGPU (Dawn) asks of Direct3D 12
 * that Arctic's vkd3d-proton 3.0.1 did not do on a GTX 1050 (Pascal) or an
 * Intel HD 630 (Gen9), the cards without VK_EXT_device_generated_commands:
 *
 *   1. ExecuteIndirect with a command signature of root constants + a
 *      DRAW_INDEXED (Dawn's drawIndexedIndirect when the shader reads
 *      instance_index): the constants hold firstVertex/firstInstance;
 *   2. a render pass that ends with an MSAA resolve whose SrcRect is
 *      {0,0,0,0}, "the whole subresource" (Dawn's every MSAA pass).
 *
 *   d3d12indirect
 *
 * Draws a quad into a 4x4 target each way and reads a pixel back: green is
 * right, blue = nothing drawn (or nothing resolved), red = the constants did
 * not arrive. Exit code = number of failed checks.
 *
 * d3d12.dll is loaded the ordinary way, so one next to the exe goes before
 * System32's: a build of vkd3d-proton can be tried before it is installed.
 * Run it on the discrete card: GpuPreference=2 in UserGpuPreferences.
 *
 * Build (arctic-build):
 *   x86_64-w64-mingw32-gcc -O2 -o d3d12indirect.exe d3d12indirect.c -ldxguid -luuid */

#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS   /* methods that return a struct (GetCPUDescriptorHandleForHeapStart) */
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdio.h>

typedef HRESULT (WINAPI *PFN_COMPILE)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *,
                                      const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList4 *list;
static ID3D12Fence *fence;
static UINT64 fence_value;
static HANDLE fence_event;

static const char hlsl[] =
    "cbuffer C : register(b0) { uint first_vertex; uint first_instance; };\n"
    "struct VO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VO vs(float2 p : POSITION, uint iid : SV_InstanceID)\n"
    "{\n"
    "    VO o; o.pos = float4(p, 0.5, 1.0);\n"
    "    o.col = (iid + first_instance == 3) ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);\n"
    "    return o;\n"
    "}\n"
    "float4 ps(VO i) : SV_Target { return i.col; }\n";

static void wait_gpu(void)
{
    ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
    ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event);
    WaitForSingleObject(fence_event, 10000);
}

static ID3D12Resource *buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static ID3D12Resource *target(UINT samples, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                               { samples, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R8G8B8A8_UNORM, { 0, 0, 1, 1 } };
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, &cv, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList4_ResourceBarrier(list, 1, &b);
}

/* copies pixel (1,1) of a 4x4 RGBA8 target (in COPY_SOURCE) and says what it is */
static int verdict(const char *what, ID3D12Resource *tex, ID3D12Resource *readback)
{
    D3D12_TEXTURE_COPY_LOCATION src = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    unsigned char *p;
    int ok;

    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = 4;
    dst.PlacedFootprint.Footprint.Height = 4;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList4_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    ID3D12GraphicsCommandList4_Close(list);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    wait_gpu();
    ID3D12Resource_Map(readback, 0, NULL, (void **)&p);
    p += 256 + 4;
    ok = p[0] < 30 && p[1] > 220 && p[2] < 30;
    printf("%-58s %s  (%u,%u,%u,%u)\n", what, ok ? "OK" : (p[2] > 220 ? "FAIL: nothing" : (p[0] > 220 ? "FAIL: constants lost" : "FAIL")),
           p[0], p[1], p[2], p[3]);
    ID3D12Resource_Unmap(readback, 0, NULL);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList4_Reset(list, alloc, NULL);
    return !ok;
}

int main(void)
{
    HMODULE d3d12 = LoadLibraryA("d3d12.dll"), comp = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3D12_CREATE_DEVICE create_device = (void *)GetProcAddress(d3d12, "D3D12CreateDevice");
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = (void *)GetProcAddress(d3d12, "D3D12SerializeRootSignature");
    PFN_COMPILE compile = comp ? (void *)GetProcAddress(comp, "D3DCompile") : NULL;
    ID3DBlob *vs, *ps, *rs_blob, *err;
    ID3D12RootSignature *rs;
    ID3D12PipelineState *pso1, *pso4;
    ID3D12CommandSignature *sig;
    ID3D12DescriptorHeap *rtv_heap;
    ID3D12Resource *vb, *ib, *args, *rb, *rt1, *rt4, *resolved;
    char path[MAX_PATH];
    int failed = 0;
    void *p;

    GetModuleFileNameA(d3d12, path, sizeof(path));
    printf("d3d12.dll: %s\n", path);
    if (!create_device || !compile) { printf("no D3D12CreateDevice or D3DCompile\n"); return 100; }
    if (FAILED(create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev))) { printf("no device\n"); return 100; }
    {
        LUID luid = ID3D12Device_GetAdapterLuid(dev);
        printf("adapter LUID %08lx:%08lx\n", luid.HighPart, luid.LowPart);
    }

    {
        D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
        ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
        ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList4, (void **)&list);
        ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
        fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    }

    if (FAILED(compile(hlsl, sizeof(hlsl) - 1, "t", NULL, NULL, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(compile(hlsl, sizeof(hlsl) - 1, "t", NULL, NULL, "ps", "ps_5_0", 0, 0, &ps, &err)))
    { printf("HLSL: %s\n", (char *)ID3D10Blob_GetBufferPointer(err)); return 100; }

    /* root signature: two root constants at b0, as Dawn has for first vertex/instance */
    {
        D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS };
        D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
        param.Constants.ShaderRegister = 0;
        param.Constants.Num32BitValues = 2;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err);
        ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rs_blob), ID3D10Blob_GetBufferSize(rs_blob),
                                         &IID_ID3D12RootSignature, (void **)&rs);
    }
    {
        D3D12_INPUT_ELEMENT_DESC ie = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = { 0 };
        pd.pRootSignature = rs;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = ~0u;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.InputLayout.pInputElementDescs = &ie; pd.InputLayout.NumElements = 1;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso1);
        pd.SampleDesc.Count = 4;
        ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso4);
    }
    /* Dawn's indirect signature: 2 root constants, then the draw */
    {
        D3D12_INDIRECT_ARGUMENT_DESC ad[2] = { { D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT }, { D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED } };
        D3D12_COMMAND_SIGNATURE_DESC sd = { 2 * sizeof(UINT) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), 2, ad, 0 };
        ad[0].Constant.RootParameterIndex = 0;
        ad[0].Constant.DestOffsetIn32BitValues = 0;
        ad[0].Constant.Num32BitValuesToSet = 2;
        if (FAILED(ID3D12Device_CreateCommandSignature(dev, &sd, rs, &IID_ID3D12CommandSignature, (void **)&sig)))
        { printf("CreateCommandSignature failed\n"); return 100; }
    }

    {
        static const float quad[] = { -1, -1, 1, -1, 1, 1, -1, 1 };
        static const UINT idx[] = { 0, 1, 2, 0, 2, 3 };
        static const UINT arg[] = { 0, 3, 6, 1, 0, 0, 0 };   /* first vertex 0, first instance 3; 6 indices, 1 instance */
        vb = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(quad), D3D12_RESOURCE_STATE_GENERIC_READ);
        ib = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(idx), D3D12_RESOURCE_STATE_GENERIC_READ);
        args = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(arg), D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource_Map(vb, 0, NULL, &p); memcpy(p, quad, sizeof(quad)); ID3D12Resource_Unmap(vb, 0, NULL);
        ID3D12Resource_Map(ib, 0, NULL, &p); memcpy(p, idx, sizeof(idx)); ID3D12Resource_Unmap(ib, 0, NULL);
        ID3D12Resource_Map(args, 0, NULL, &p); memcpy(p, arg, sizeof(arg)); ID3D12Resource_Unmap(args, 0, NULL);
    }
    rb = buffer(D3D12_HEAP_TYPE_READBACK, 256 * 4, D3D12_RESOURCE_STATE_COPY_DEST);
    rt1 = target(1, D3D12_RESOURCE_STATE_RENDER_TARGET);
    rt4 = target(4, D3D12_RESOURCE_STATE_RENDER_TARGET);
    resolved = target(1, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv1 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap), rtv4 = rtv1;
    rtv4.ptr += ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Device_CreateRenderTargetView(dev, rt1, NULL, rtv1);
    ID3D12Device_CreateRenderTargetView(dev, rt4, NULL, rtv4);

    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), 32, 8 };
    D3D12_INDEX_BUFFER_VIEW ibv = { ID3D12Resource_GetGPUVirtualAddress(ib), 24, DXGI_FORMAT_R32_UINT };
    D3D12_VIEWPORT vp = { 0, 0, 4, 4, 0, 1 };
    D3D12_RECT sc = { 0, 0, 4, 4 };
    static const float blue[4] = { 0, 0, 1, 1 };
#define STATE(pso) do { \
        ID3D12GraphicsCommandList4_SetGraphicsRootSignature(list, rs); \
        ID3D12GraphicsCommandList4_SetPipelineState(list, pso); \
        ID3D12GraphicsCommandList4_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); \
        ID3D12GraphicsCommandList4_IASetVertexBuffers(list, 0, 1, &vbv); \
        ID3D12GraphicsCommandList4_IASetIndexBuffer(list, &ibv); \
        ID3D12GraphicsCommandList4_RSSetViewports(list, 1, &vp); \
        ID3D12GraphicsCommandList4_RSSetScissorRects(list, 1, &sc); } while (0)

    /* 0: the same draw direct, constants set by the CPU: the baseline */
    STATE(pso1);
    ID3D12GraphicsCommandList4_OMSetRenderTargets(list, 1, &rtv1, FALSE, NULL);
    ID3D12GraphicsCommandList4_ClearRenderTargetView(list, rtv1, blue, 0, NULL);
    {
        UINT c[2] = { 0, 3 };
        ID3D12GraphicsCommandList4_SetGraphicsRoot32BitConstants(list, 0, 2, c, 0);
    }
    ID3D12GraphicsCommandList4_DrawIndexedInstanced(list, 6, 1, 0, 0, 0);
    barrier(rt1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    failed += verdict("direct draw, root constants from the CPU", rt1, rb);

    /* 1: ExecuteIndirect, constants from the argument buffer */
    barrier(rt1, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    STATE(pso1);
    ID3D12GraphicsCommandList4_OMSetRenderTargets(list, 1, &rtv1, FALSE, NULL);
    ID3D12GraphicsCommandList4_ClearRenderTargetView(list, rtv1, blue, 0, NULL);
    ID3D12GraphicsCommandList4_ExecuteIndirect(list, sig, 1, args, 0, NULL, 0);
    barrier(rt1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    failed += verdict("ExecuteIndirect [root constants + DRAW_INDEXED]", rt1, rb);

    /* 2: render pass on 4x MSAA, cleared, resolved with SrcRect {0,0,0,0} */
    {
        D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS sub = { 0, 0, 0, 0, { 0, 0, 0, 0 } };
        D3D12_RENDER_PASS_RENDER_TARGET_DESC rtd = { rtv4 };
        UINT c[2] = { 0, 3 };
        rtd.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
        rtd.BeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        memcpy(rtd.BeginningAccess.Clear.ClearValue.Color, blue, sizeof(blue));
        rtd.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
        rtd.EndingAccess.Resolve.pSrcResource = rt4;
        rtd.EndingAccess.Resolve.pDstResource = resolved;
        rtd.EndingAccess.Resolve.SubresourceCount = 1;
        rtd.EndingAccess.Resolve.pSubresourceParameters = &sub;
        rtd.EndingAccess.Resolve.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtd.EndingAccess.Resolve.ResolveMode = D3D12_RESOLVE_MODE_AVERAGE;
        rtd.EndingAccess.Resolve.PreserveResolveSource = FALSE;
        ID3D12GraphicsCommandList4_BeginRenderPass(list, 1, &rtd, NULL, D3D12_RENDER_PASS_FLAG_NONE);
        STATE(pso4);
        ID3D12GraphicsCommandList4_SetGraphicsRoot32BitConstants(list, 0, 2, c, 0);
        ID3D12GraphicsCommandList4_DrawIndexedInstanced(list, 6, 1, 0, 0, 0);
        ID3D12GraphicsCommandList4_EndRenderPass(list);
    }
    barrier(resolved, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    failed += verdict("render pass MSAA 4x, resolve with SrcRect {0,0,0,0}", resolved, rb);

    printf("%s\n", failed ? "FAILED" : "all right");
    return failed;
}
