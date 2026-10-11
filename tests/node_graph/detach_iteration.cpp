/* Device-free live-detachment regression: c++ -std=c++17 -pthread detach_iteration.cpp -lm -o detach_iteration */
#define MA_NO_DEVICE_IO
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENGINE
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "../../miniaudio.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <cstdio>
struct Gate { std::mutex mutex; std::condition_variable changed; bool entered=false, resume=false; };
struct Source { ma_node_base base{}; Gate *gate=nullptr; ma_uint64 frames=0; };
static void process(ma_node *node,const float **,ma_uint32 *,float **out,ma_uint32 *count) {
    auto &source=*reinterpret_cast<Source *>(node);
    if (source.gate) {
        auto &gate=*source.gate; std::unique_lock<std::mutex> lock(gate.mutex);
        gate.entered=true; gate.changed.notify_all();
        gate.changed.wait(lock,[&]{return gate.resume;});
    }
    for(ma_uint32 i=0;i<*count;++i) out[0][i]=source.gate ? 0.f : 1.f;
    source.frames+=*count;
}
static ma_node_vtable vtable{process,nullptr,0,1,0};
static void test_detach(bool reroute, bool adjacent) {
    ma_node_graph graph; auto graphConfig=ma_node_graph_config_init(1);
    graphConfig.processingSizeInFrames=64;
    assert(ma_node_graph_init(&graphConfig,nullptr,&graph)==MA_SUCCESS);
    ma_uint32 channels=1; auto config=ma_node_config_init();
    config.vtable=&vtable; config.pOutputChannels=&channels;
    Gate gate; Source stable, neighbor, removed; removed.gate=&gate;
    assert(ma_node_init(&graph,&config,nullptr,&stable)==MA_SUCCESS);
    assert(ma_node_init(&graph,&config,nullptr,&removed)==MA_SUCCESS);
    if(adjacent) assert(ma_node_init(&graph,&config,nullptr,&neighbor)==MA_SUCCESS);
    ma_splitter_node destination; auto splitConfig=ma_splitter_node_config_init(1);
    assert(ma_splitter_node_init(&graph,&splitConfig,nullptr,&destination)==MA_SUCCESS);
    assert(ma_node_attach_output_bus(&stable,0,ma_node_graph_get_endpoint(&graph),0)==MA_SUCCESS);
    if(adjacent) assert(ma_node_attach_output_bus(&neighbor,0,ma_node_graph_get_endpoint(&graph),0)==MA_SUCCESS);
    assert(ma_node_attach_output_bus(&removed,0,ma_node_graph_get_endpoint(&graph),0)==MA_SUCCESS);
    float output[64]; ma_uint64 read=0;
    std::thread reader([&]{assert(ma_node_graph_read_pcm_frames(&graph,output,64,&read)==MA_SUCCESS);});
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        assert(gate.changed.wait_for(lock,std::chrono::seconds(5),[&]{return gate.entered;}));
    }
    auto detach=std::async(std::launch::async,[&]{
        return reroute ? ma_node_attach_output_bus(&removed,0,&destination,0) : ma_node_detach_output_bus(&removed,0);
    });
    // Removal must wait while the reader still holds this bus. Keep the callback
    // paused long enough for the removal thread to reach that wait, then finish it.
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(ma_node_output_bus_is_attached(&removed.base.pOutputBuses[0])) {
        assert(std::chrono::steady_clock::now()<deadline); std::this_thread::yield();
    }
    assert(detach.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
    // A neighboring removal must update the still-linked current entry too.
    if(adjacent) ma_node_uninit(&neighbor,nullptr);
    { std::lock_guard<std::mutex> lock(gate.mutex); gate.resume=true; gate.changed.notify_all(); }
    reader.join(); assert(detach.get()==MA_SUCCESS);
    assert(read==64);
    // Removing the current list entry must not skip unrelated sources after it.
    assert(stable.frames==64);
    for(float sample:output) assert(sample==1.f);
    ma_node_uninit(&removed,nullptr); ma_node_uninit(&stable,nullptr);
    ma_splitter_node_uninit(&destination,nullptr); ma_node_graph_uninit(&graph,nullptr);
}
static void test_reroute_selection() {
    ma_node_graph graph; auto graphConfig=ma_node_graph_config_init(1);
    assert(ma_node_graph_init(&graphConfig,nullptr,&graph)==MA_SUCCESS);
    ma_uint32 channels=1; auto config=ma_node_config_init();
    config.vtable=&vtable; config.pOutputChannels=&channels;
    Source source; assert(ma_node_init(&graph,&config,nullptr,&source)==MA_SUCCESS);
    ma_splitter_node destination; auto splitConfig=ma_splitter_node_config_init(1);
    assert(ma_splitter_node_init(&graph,&splitConfig,nullptr,&destination)==MA_SUCCESS);
    assert(ma_node_attach_output_bus(&source,0,ma_node_graph_get_endpoint(&graph),0)==MA_SUCCESS);
    auto *oldInput=&graph.endpoint.pInputBuses[0];
    // Pause a selector before it increments any output reference. Rerouting must
    // drain this old input's selection window, rather than the new input's.
    ma_node_input_bus_next_begin(oldInput);
    std::promise<void> started; auto startedFuture=started.get_future();
    auto reroute=std::async(std::launch::async,[&]{ started.set_value(); return ma_node_attach_output_bus(&source,0,&destination,0); });
    startedFuture.wait();
    assert(reroute.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
    ma_node_input_bus_next_end(oldInput);
    assert(reroute.get()==MA_SUCCESS);
    ma_node_uninit(&source,nullptr); ma_splitter_node_uninit(&destination,nullptr);
    ma_node_graph_uninit(&graph,nullptr);
}
int main() { test_detach(false,false); test_detach(true,false); test_detach(false,true); test_detach(true,true); test_reroute_selection(); std::puts("PASS: live detach and reroute preserve the current input iteration"); }
