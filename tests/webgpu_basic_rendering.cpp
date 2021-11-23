// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// Based on https://github.com/kainino0x/webgpu-cross-platform-demo

#include <webgpu/webgpu_cpp.h>

#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgpu.h>

void GetDevice(void (*callback)(wgpu::Device)) {
    // Left as null (until supported in Emscripten)
    static const WGPUInstance instance = nullptr;

    wgpuInstanceRequestAdapter(instance, nullptr, [](WGPURequestAdapterStatus status, WGPUAdapter adapter, const char* message, void* userdata) {
        if (message) {
            printf("wgpuInstanceRequestAdapter: %s\n", message);
        }
        if (status == WGPURequestAdapterStatus_Unavailable) {
            printf("WebGPU unavailable; exiting cleanly\n");
            // exit(0) (rather than emscripten_force_exit(0)) ensures there is no dangling keepalive.
            exit(0);
        }
        assert(status == WGPURequestAdapterStatus_Success);

        wgpuAdapterRequestDevice(adapter, nullptr, [](WGPURequestDeviceStatus status, WGPUDevice dev, const char* message, void* userdata) {
            if (message) {
                printf("wgpuAdapterRequestDevice: %s\n", message);
            }
            assert(status == WGPURequestDeviceStatus_Success);

            wgpu::Device device = wgpu::Device::Acquire(dev);
            reinterpret_cast<void (*)(wgpu::Device)>(userdata)(device);
        }, userdata);
    }, reinterpret_cast<void*>(callback));
}

static const char shaderCode[] = R"(
    [[stage(vertex)]]
    fn main_v([[builtin(vertex_index)]] idx: u32) -> [[builtin(position)]] vec4<f32> {
        var pos = array<vec2<f32>, 3>(
            vec2<f32>(0.0, 0.5), vec2<f32>(-0.5, -0.5), vec2<f32>(0.5, -0.5));
        return vec4<f32>(pos[idx], 0.0, 1.0);
    }

    [[stage(fragment)]]
    fn main_f() -> [[location(0)]] vec4<f32> {
        return vec4<f32>(0.0, 0.502, 1.0, 1.0); // 0x80/0xff ~= 0.502
    }
)";

static wgpu::Device device;
static wgpu::Queue queue;
static wgpu::Buffer readbackBuffer;
static wgpu::RenderPipeline pipeline;
static int testsCompleted = 0;

void init() {
    device.SetUncapturedErrorCallback(
        [](WGPUErrorType errorType, const char* message, void*) {
            printf("%d: %s\n", errorType, message);
        }, nullptr);

    queue = device.GetQueue();

    wgpu::ShaderModule shaderModule{};
    {
        wgpu::ShaderModuleWGSLDescriptor wgslDesc{};
        wgslDesc.source = shaderCode;

        wgpu::ShaderModuleDescriptor descriptor{};
        descriptor.nextInChain = &wgslDesc;
        shaderModule = device.CreateShaderModule(&descriptor);
    }

    {
        wgpu::BindGroupLayoutDescriptor bglDesc{};
        auto bgl = device.CreateBindGroupLayout(&bglDesc);
        wgpu::BindGroupDescriptor desc{};
        desc.layout = bgl;
        desc.entryCount = 0;
        desc.entries = nullptr;
        device.CreateBindGroup(&desc);
    }

    {
        wgpu::PipelineLayoutDescriptor pl{};
        pl.bindGroupLayoutCount = 0;
        pl.bindGroupLayouts = nullptr;

        wgpu::ColorTargetState colorTargetState{};
        colorTargetState.format = wgpu::TextureFormat::BGRA8Unorm;

        wgpu::FragmentState fragmentState{};
        fragmentState.module = shaderModule;
        fragmentState.entryPoint = "main_f";
        fragmentState.targetCount = 1;
        fragmentState.targets = &colorTargetState;

        wgpu::RenderPipelineDescriptor descriptor{};
        descriptor.layout = device.CreatePipelineLayout(&pl);
        descriptor.vertex.module = shaderModule;
        descriptor.vertex.entryPoint = "main_v";
        descriptor.fragment = &fragmentState;
        descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pipeline = device.CreateRenderPipeline(&descriptor);
    }
}

void render(wgpu::TextureView view) {
    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = view;
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearColor = {0, 0, 0, 1};

    wgpu::RenderPassDescriptor renderpass{};
    renderpass.colorAttachmentCount = 1;
    renderpass.colorAttachments = &attachment;

    wgpu::CommandBuffer commands;
    {
        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        {
            wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&renderpass);
            pass.SetPipeline(pipeline);
            pass.Draw(3, 1, 0, 0);
            pass.EndPass();
        }
        commands = encoder.Finish();
    }

    queue.Submit(1, &commands);
}

void issueContentsCheck(const char* functionName,
        wgpu::Buffer readbackBuffer, uint32_t expectData) {
    struct UserData {
        const char* functionName;
        wgpu::Buffer readbackBuffer;
        uint32_t expectData;
    };

    UserData* userdata = new UserData;
    userdata->functionName = functionName;
    userdata->readbackBuffer = readbackBuffer;
    userdata->expectData = expectData;

    readbackBuffer.MapAsync(
        wgpu::MapMode::Read, 0, 4,
        [](WGPUBufferMapAsyncStatus status, void* vp_userdata) {
            assert(status == WGPUBufferMapAsyncStatus_Success);
            std::unique_ptr<UserData> userdata(reinterpret_cast<UserData*>(vp_userdata));

            const void* ptr = userdata->readbackBuffer.GetConstMappedRange();

            printf("%s: readback -> %p%s\n", userdata->functionName,
                    ptr, ptr ? "" : " <------- FAILED");
            assert(ptr != nullptr);
            uint32_t readback = static_cast<const uint32_t*>(ptr)[0];
            userdata->readbackBuffer.Unmap();
            printf("  got %08x, expected %08x%s\n",
                readback, userdata->expectData,
                readback == userdata->expectData ? "" : " <------- FAILED");

            testsCompleted++;
        }, userdata);
}

void doCopyTestMappedAtCreation(bool useRange) {
    static constexpr uint32_t kValue = 0x05060708;
    size_t size = useRange ? 12 : 4;
    wgpu::Buffer src;
    {
        wgpu::BufferDescriptor descriptor{};
        descriptor.size = size;
        descriptor.usage = wgpu::BufferUsage::CopySrc;
        descriptor.mappedAtCreation = true;
        src = device.CreateBuffer(&descriptor);
    }
    size_t offset = useRange ? 8 : 0;
    uint32_t* ptr = static_cast<uint32_t*>(useRange ?
            src.GetMappedRange(offset, 4) :
            src.GetMappedRange());
    printf("%s: getMappedRange -> %p%s\n", __FUNCTION__,
            ptr, ptr ? "" : " <------- FAILED");
    assert(ptr != nullptr);
    *ptr = kValue;
    src.Unmap();

    wgpu::Buffer dst;
    {
        wgpu::BufferDescriptor descriptor{};
        descriptor.size = 4;
        descriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        dst = device.CreateBuffer(&descriptor);
    }

    wgpu::CommandBuffer commands;
    {
        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        encoder.CopyBufferToBuffer(src, offset, dst, 0, 4);
        commands = encoder.Finish();
    }
    queue.Submit(1, &commands);

    issueContentsCheck(__FUNCTION__, dst, kValue);
}

void doCopyTestMapAsync(bool useRange) {
    static constexpr uint32_t kValue = 0x01020304;
    size_t size = useRange ? 12 : 4;
    wgpu::Buffer src;
    {
        wgpu::BufferDescriptor descriptor{};
        descriptor.size = size;
        descriptor.usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc;
        src = device.CreateBuffer(&descriptor);
    }
    size_t offset = useRange ? 8 : 0;

    struct UserData {
        const char* functionName;
        bool useRange;
        size_t offset;
        wgpu::Buffer src;
    };

    UserData* userdata = new UserData;
    userdata->functionName = __FUNCTION__;
    userdata->useRange = useRange;
    userdata->offset = offset;
    userdata->src = src;

    src.MapAsync(wgpu::MapMode::Write, offset, 4,
        [](WGPUBufferMapAsyncStatus status, void* vp_userdata) {
            assert(status == WGPUBufferMapAsyncStatus_Success);
            std::unique_ptr<UserData> userdata(reinterpret_cast<UserData*>(vp_userdata));

            uint32_t* ptr = static_cast<uint32_t*>(userdata->useRange ?
                    userdata->src.GetMappedRange(userdata->offset, 4) :
                    userdata->src.GetMappedRange());
            printf("%s: getMappedRange -> %p%s\n", userdata->functionName,
                    ptr, ptr ? "" : " <------- FAILED");
            assert(ptr != nullptr);
            *ptr = kValue;
            userdata->src.Unmap();

            wgpu::Buffer dst;
            {
                wgpu::BufferDescriptor descriptor{};
                descriptor.size = 4;
                descriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
                dst = device.CreateBuffer(&descriptor);
            }

            wgpu::CommandBuffer commands;
            {
                wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
                encoder.CopyBufferToBuffer(userdata->src, userdata->offset, dst, 0, 4);
                commands = encoder.Finish();
            }
            queue.Submit(1, &commands);

            issueContentsCheck(userdata->functionName, dst, kValue);
        }, userdata);
}

void doRenderTest() {
    wgpu::Texture readbackTexture;
    {
        wgpu::TextureDescriptor descriptor{};
        descriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
        descriptor.size = {1, 1, 1};
        descriptor.format = wgpu::TextureFormat::BGRA8Unorm;
        readbackTexture = device.CreateTexture(&descriptor);
    }
    render(readbackTexture.CreateView());

    {
        wgpu::BufferDescriptor descriptor{};
        descriptor.size = 4;
        descriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;

        readbackBuffer = device.CreateBuffer(&descriptor);
    }

    wgpu::CommandBuffer commands;
    {
        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        wgpu::ImageCopyTexture src{};
        src.texture = readbackTexture;
        src.origin = {0, 0, 0};
        wgpu::ImageCopyBuffer dst{};
        dst.buffer = readbackBuffer;
        dst.layout.bytesPerRow = 256;
        wgpu::Extent3D extent = {1, 1, 1};
        encoder.CopyTextureToBuffer(&src, &dst, &extent);
        commands = encoder.Finish();
    }
    queue.Submit(1, &commands);

    // Check the color value encoded in the shader makes it out correctly.
    static const uint32_t expectData = 0xff0080ff;
    issueContentsCheck(__FUNCTION__, readbackBuffer, expectData);
}

wgpu::SwapChain swapChain;

void frame() {
    wgpu::TextureView backbuffer = swapChain.GetCurrentTextureView();
    render(backbuffer);

    // TODO: Read back from the canvas with drawImage() (or something) and
    // check the result.

    emscripten_cancel_main_loop();

    // exit(0) (rather than emscripten_force_exit(0)) ensures there is no dangling keepalive.
    exit(0);
}

void run() {
    init();

    doCopyTestMappedAtCreation(false);
    doCopyTestMappedAtCreation(true);
    doCopyTestMapAsync(false);
    doCopyTestMapAsync(true);
    doRenderTest();

    {
        wgpu::SurfaceDescriptorFromCanvasHTMLSelector canvasDesc{};
        canvasDesc.selector = "#canvas";

        wgpu::SurfaceDescriptor surfDesc{};
        surfDesc.nextInChain = &canvasDesc;
        wgpu::Instance instance{};  // null instance
        wgpu::Surface surface = instance.CreateSurface(&surfDesc);

        wgpu::SwapChainDescriptor scDesc{};
        scDesc.usage = wgpu::TextureUsage::RenderAttachment;
        scDesc.format = wgpu::TextureFormat::BGRA8Unorm;
        scDesc.width = 300;
        scDesc.height = 150;
        scDesc.presentMode = wgpu::PresentMode::Fifo;
        swapChain = device.CreateSwapChain(surface, &scDesc);
    }
    emscripten_set_main_loop(frame, 0, false);
}

int main() {
    GetDevice([](wgpu::Device dev) {
        device = dev;
        run();
    });

    // The test result will be reported when the main_loop completes.
    // emscripten_exit_with_live_runtime isn't needed because the WebGPU
    // callbacks should all automatically keep the runtime alive until
    // emscripten_set_main_loop, and that should keep it alive until
    // emscripten_cancel_main_loop.
    //
    // This code is returned when the runtime exits unless something else sets it, like exit(0).
    return 99;
}
