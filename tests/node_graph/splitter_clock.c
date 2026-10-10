/* Standalone, device-free regression: cc -std=c89 splitter_clock.c -lm -o splitter_clock */
#define MA_NO_DEVICE_IO
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENGINE
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "../../miniaudio.h"
#include <assert.h>
#include <stdio.h>

typedef struct { ma_node_base base; ma_uint64 cursor; } ramp_node;
static float sample_at(ma_uint64 frame) { return (float)((frame % 100000) + 1) * 0.000001f; }
static void ramp_process(ma_node* node, const float** in, ma_uint32* count_in, float** out, ma_uint32* count_out)
{
    ramp_node* source = (ramp_node*)node;
    ma_uint32 f;
    (void)in; (void)count_in;
    for (f = 0; f < *count_out; ++f) out[0][f] = sample_at(source->cursor++);
}
static ma_node_vtable ramp_vtable = { ramp_process, NULL, 0, 1, 0 };
static void pass_process(ma_node* node, const float** in, ma_uint32* count_in, float** out, ma_uint32* count_out)
{ (void)node; (void)in; (void)count_in; (void)out; (void)count_out; }
static ma_node_vtable pass_vtable = { pass_process, NULL, 1, 1, MA_NODE_FLAG_PASSTHROUGH };
static void check_frames(ma_node_graph* graph, ma_uint32 frames, ma_uint64* cursor, float gain)
{
    float buffer[2048];
    ma_uint64 read;
    ma_uint32 f;
    assert(frames <= 2048);
    assert(ma_node_graph_read_pcm_frames(graph, buffer, frames, &read) == MA_SUCCESS && read == frames);
    for (f = 0; f < frames; ++f) {
        if (fabs(buffer[f] - sample_at(*cursor + f) * gain) > 0.0000001f) {
            fprintf(stderr, "frame %llu: got %.9f expected %.9f (gain %.1f)\n", (unsigned long long)(*cursor+f), buffer[f], sample_at(*cursor+f)*gain, gain);
            assert(0);
        }
    }
    *cursor += frames;
}
static void test_clock(ma_uint32 quantum)
{
    ma_node_graph graph;
    ma_node_graph_config graph_config = ma_node_graph_config_init(1);
    ramp_node source;
    ma_splitter_node splitter;
    ma_splitter_node_config split_config = ma_splitter_node_config_init(1);
    ma_node_base branch;
    ma_node_config config = ma_node_config_init();
    ma_uint32 channels = 1;
    ma_uint32 requests[] = {1, 7, 63, 257, 1025};
    ma_uint64 cursor = 0;
    ma_uint32 i;
    graph_config.processingSizeInFrames = quantum;
    assert(ma_node_graph_init(&graph_config, NULL, &graph) == MA_SUCCESS);
    if (quantum == 0) quantum = ma_node_graph_get_processing_size_in_frames(&graph);
    MA_ZERO_OBJECT(&source);
    config.vtable = &ramp_vtable; config.pOutputChannels = &channels;
    assert(ma_node_init(&graph, &config, NULL, &source.base) == MA_SUCCESS);
    split_config.outputBusCount = 3;
    assert(ma_splitter_node_init(&graph, &split_config, NULL, &splitter) == MA_SUCCESS);
    config.vtable = &pass_vtable; config.pInputChannels = &channels;
    assert(ma_node_init(&graph, &config, NULL, &branch) == MA_SUCCESS);
    assert(ma_node_attach_output_bus(&source, 0, &splitter, 0) == MA_SUCCESS);
    assert(ma_node_attach_output_bus(&splitter, 0, ma_node_graph_get_endpoint(&graph), 0) == MA_SUCCESS);
    assert(ma_node_attach_output_bus(&branch, 0, ma_node_graph_get_endpoint(&graph), 0) == MA_SUCCESS);
    /* Mutations occur at graph boundaries; arbitrary client reads are covered below. */
    check_frames(&graph, quantum ? quantum * 2 : 480, &cursor, 1);
    assert(ma_node_attach_output_bus(&splitter, 1, &branch, 0) == MA_SUCCESS);
    for (i = 0; i < sizeof(requests)/sizeof(requests[0]); ++i) check_frames(&graph, requests[i], &cursor, 2);
    if (quantum && cursor % quantum) check_frames(&graph, quantum - (ma_uint32)(cursor % quantum), &cursor, 2);
    assert(ma_node_set_state(&branch, ma_node_state_stopped) == MA_SUCCESS);
    check_frames(&graph, quantum ? quantum * 2 : 480, &cursor, 1);
    assert(ma_node_set_state(&branch, ma_node_state_started) == MA_SUCCESS);
    check_frames(&graph, quantum ? quantum * 2 : 480, &cursor, 2);
    assert(ma_node_detach_output_bus(&splitter, 1) == MA_SUCCESS);
    check_frames(&graph, quantum ? quantum : 480, &cursor, 1);
    assert(ma_node_attach_output_bus(&splitter, 2, &branch, 0) == MA_SUCCESS);
    check_frames(&graph, quantum ? quantum : 480, &cursor, 2);
    ma_node_uninit(&branch, NULL);
    ma_splitter_node_uninit(&splitter, NULL);
    ma_node_uninit(&source, NULL);
    ma_node_graph_uninit(&graph, NULL);
}
static void test_scheduled_branch(void)
{
    ma_node_graph graph;
    ma_node_graph_config graph_config = ma_node_graph_config_init(1);
    ramp_node source;
    ma_splitter_node splitter;
    ma_splitter_node_config split_config = ma_splitter_node_config_init(1);
    ma_node_base branch;
    ma_node_config config = ma_node_config_init();
    ma_uint32 channels = 1;
    ma_uint32 f;
    ma_uint64 read;
    float buffer[256];
    graph_config.processingSizeInFrames = 64;
    assert(ma_node_graph_init(&graph_config, NULL, &graph) == MA_SUCCESS);
    MA_ZERO_OBJECT(&source);
    config.vtable = &ramp_vtable; config.pOutputChannels = &channels;
    assert(ma_node_init(&graph, &config, NULL, &source.base) == MA_SUCCESS);
    assert(ma_splitter_node_init(&graph, &split_config, NULL, &splitter) == MA_SUCCESS);
    config.vtable = &pass_vtable; config.pInputChannels = &channels;
    assert(ma_node_init(&graph, &config, NULL, &branch) == MA_SUCCESS);
    assert(ma_node_set_state_time(&branch, ma_node_state_started, 17) == MA_SUCCESS);
    assert(ma_node_set_state_time(&branch, ma_node_state_stopped, 113) == MA_SUCCESS);
    assert(ma_node_attach_output_bus(&source, 0, &splitter, 0) == MA_SUCCESS);
    assert(ma_node_attach_output_bus(&splitter, 0, ma_node_graph_get_endpoint(&graph), 0) == MA_SUCCESS);
    assert(ma_node_attach_output_bus(&splitter, 1, &branch, 0) == MA_SUCCESS);
    assert(ma_node_set_output_bus_volume(&splitter, 1, 0.5f) == MA_SUCCESS);
    /* Last attachment is visited first: the partial branch initiates the shared read. */
    assert(ma_node_attach_output_bus(&branch, 0, ma_node_graph_get_endpoint(&graph), 0) == MA_SUCCESS);
    assert(ma_node_graph_read_pcm_frames(&graph, buffer, 256, &read) == MA_SUCCESS && read == 256);
    for (f = 0; f < 256; ++f) {
        float gain = f >= 17 && f < 113 ? 1.5f : 1;
        assert(fabs(buffer[f] - sample_at(f) * gain) < 0.0000001f);
    }
    assert(source.cursor == 256); /* Each source frame is pulled once, despite two outputs. */
    assert(ma_node_get_time(&splitter) == 256);
    ma_node_uninit(&branch, NULL);
    ma_splitter_node_uninit(&splitter, NULL);
    ma_node_uninit(&source, NULL);
    ma_node_graph_uninit(&graph, NULL);
}

int main(void)
{
    ma_uint32 quanta[] = {0, 1, 64, 256, 480, 1024};
    ma_uint32 i;
    for (i = 0; i < sizeof(quanta)/sizeof(quanta[0]); ++i) test_clock(quanta[i]);
    test_scheduled_branch();
    puts("PASS: live splitter attach, partial reads, pause/resume, detach and unused output reuse");
    return 0;
}
