/*
 * Copyright 2019 Xilinx, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "/proj/gw/Xilinx/Vivado/2019.2/include/gmp.h"
#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <ap_int.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "hls_stream.h"
#include "lz_compress.hpp"
#include "lz_optional.hpp"
#include "snappy_compress.hpp"
#include "axi_to_stream.hpp"
#include "stream_to_axi.hpp"

#define PARALLEL_BLOCK 1
#ifdef LARGE_LIT_RANGE
#define MAX_LIT_COUNT 4090
#define MAX_LIT_STREAM_SIZE 4096
#else
#define MAX_LIT_COUNT 60
#define MAX_LIT_STREAM_SIZE 64
#endif
int const c_minMatch = 4;
#define LZ_MAX_OFFSET_LIMIT 65536
#define OFFSET_WINDOW (64 * 1024)
#define MAX_MATCH_LEN 255
#define MATCH_LEN 6

const int c_snappyMaxLiteralStream = MAX_LIT_STREAM_SIZE;

typedef ap_uint<8> uintV_t;

void snappyCompressEngineRun(hls::stream<uintV_t>& inStream,
                             hls::stream<uintV_t>& snappyOut,
                             hls::stream<bool>& byte_out_eos,
                             hls::stream<uint32_t>& byte_out_size,
                             uint32_t max_lit_limit[PARALLEL_BLOCK],
                             uint32_t input_size,
                             uint32_t core_idx) {
    hls::stream<xf::compression::compressd_dt> compressdStream("compressdStream");
    hls::stream<xf::compression::compressd_dt> bestMatchStream("bestMatchStream");
    hls::stream<xf::compression::compressd_dt> boosterStream("boosterStream");

#pragma HLS STREAM variable = compressdStream depth = 8
#pragma HLS STREAM variable = bestMatchStream depth = 8
#pragma HLS STREAM variable = boosterStream depth = 8

#pragma HLS BIND_STORAGE variable = inStream type = FIFO impl = SRL
#pragma HLS BIND_STORAGE variable = outStream type = FIFO impl = SRL

// #pragma HLS RESOURCE variable = compressdStream core = FIFO_SRL
// #pragma HLS RESOURCE variable = boosterStream core = FIFO_SRL

#pragma HLS dataflow
    
    xf::compression::lzCompress<MATCH_LEN, c_minMatch, LZ_MAX_OFFSET_LIMIT>(inStream, compressdStream, input_size);
    xf::compression::lzBestMatchFilter<MATCH_LEN, OFFSET_WINDOW>(compressdStream, bestMatchStream, input_size);
    xf::compression::lzBooster<MAX_MATCH_LEN>(bestMatchStream, boosterStream, input_size);
    xf::compression::snappyCompress<MAX_LIT_COUNT, MAX_LIT_STREAM_SIZE, PARALLEL_BLOCK>(
        boosterStream, snappyOut, max_lit_limit, input_size, byte_out_eos, byte_out_size, core_idx);
}

static void read_input(uint8_t* input, hls::stream<uintV_t>& inStream, hls::stream<bool>& inStream_eos, unsigned long size) 
{
    xf::common::utils_hw::axiToStream<8>((ap_uint<8> *)input, size, inStream, inStream_eos);
}

static void write_output(uint8_t* output, hls::stream<uintV_t>& outStream, hls::stream<bool>& outStream_eos, hls::stream<uint32_t>& sizeStream)
{
    xf::common::utils_hw::streamToAxi<8>((ap_uint<8> *)output, outStream, outStream_eos);
}

void snappy_compress(uint8_t *base, unsigned long input_offset, unsigned long output_offset, unsigned long size)
{
#pragma HLS INTERFACE m_axi port=base offset=off bundle=gmem depth=4194304
    // offset output
    uint8_t *input = base + input_offset;
	uint8_t *output = base + output_offset;

    hls::stream<uintV_t> byte_in("compressIn");
    hls::stream<bool> byte_in_eos("compressIn_eos");
    hls::stream<uintV_t> byte_out("compressOut");
    hls::stream<bool> byte_out_eos("compressOut_eos");
    hls::stream<uint32_t> byte_out_size("compressOut_size");
    uint32_t max_lit_limit[PARALLEL_BLOCK];
    uint32_t core_idx;

    // read data from input array to input stream byte_in
    read_input(input, byte_in, byte_in_eos, size);
    // compression
    snappyCompressEngineRun(byte_in, byte_out, byte_out_eos, byte_out_size, max_lit_limit, size, 0);
    // write data from stream to output array
    unsigned long new_size = byte_out_size.read();
    write_output(output, byte_out, byte_out_eos, byte_out_size);
    while(!byte_in_eos.read()) {
    }
    // read byte_out to avoid this --- WARNING: Hls::stream 'compressOut' contains leftover data, which may result in RTL simulation hanging.
    uintV_t temp = byte_out.read();
    output[0] = '1';
    output[1] = '1';
    output[2] = '2';
    output[3] = '2';
    output[4] = '3';
    output[5] = '3';
    input[0] = '3';
    input[1] = '3';
    input[2] = '2';
    input[3] = '2';
    input[4] = '1';
    input[5] = '1';
}

int main(int argc, char* argv[]) {
    const char *text =
        "To evaluate our prefetcher we modelled the system using the gem5 simulator [4] in full system mode with the setup "
        "given in table 2 and the ARMv8 64-bit instruction set. Our applications are derived from existing benchmarks and "
        "libraries for graph traversal, using a range of graph sizes and characteristics. We simulate the core breadth-first search "
        "based kernels of each benchmark, skipping the graph construction phase. Our first benchmark is from the Graph 500 community [32]. "
        "We used their Kronecker graph generator for both the standard Graph 500 search benchmark and a connected components "
        "calculation. The Graph 500 benchmark is designed to represent data analytics workloads, such as 3D physics "
        "simulation. Standard inputs are too long to simulate, so we create smaller graphs with scales from 16 to 21 and edge "
        "factors from 5 to 15 (for comparison, the Graph 500 toy input has scale 26 and edge factor 16). "
        "Our prefetcher is most easily incorporated into libraries that implement graph traversal for CSR graphs. To this "
        "end, we use the Boost Graph Library (BGL) [41], a C++ templated library supporting many graph-based algorithms "
        "and graph data structures. To support our prefetcher, we added configuration instructions on constructors for CSR "
        "data structures, circular buffer queues (serving as the work list) and colour vectors (serving as the visited list). This "
        "means that any algorithm incorporating breadth-first searches on CSR graphs gains the benefits of our prefetcher without "
        "further modification. We evaluate breadth-first search, betweenness centrality and ST connectivity which all traverse "
        "graphs in this manner. To evaluate our extensions for sequential access prefetching (section 3.5) we use PageRank "
        "and sequential colouring. Inputs to the BGL algorithms are a set of real world "
        "graphs obtained from the SNAP dataset [25] chosen to represent a variety of sizes and disciplines, as shown in table 4. "
        "All are smaller than what we might expect to be processing in a real system, to enable complete simulation in a realistic "
        "time-frame, but as figure 2(a) shows, since stall rates go up for larger data structures, we expect the improvements we "
        "attain in simulation to be conservative when compared with real-world use cases.";

    size_t size = strlen(text);
    printf("size:%d\n", size);

    char *buf = (char *)malloc(3 * 1024 * 1024); // 3MB buffer
    memcpy(buf, (const uint8_t *)text, size);    // copy to internal buff
    char *input = buf;
    char *compressed = buf + 1048576;
    int j = 0;
    std::cout << std::endl
         << "before compression: " << std::endl
         << std::endl;
    for (j = 0; j < size; j++)
    {
        std::cout << input[j];
    }
    std::cout << std::endl
         << std::endl;

    unsigned long new_size = size;
    snappy_compress((uint8_t *)input, input - input, compressed - input, size);

    std::cout << std::endl
         << "after compression:" << std::endl
         << std::endl;
    for (j = 0; j < size; j++)
    {
        std::cout << input[j];
    }
    std::cout << std::endl
         << std::endl;

    for (j = 0; j < new_size; j++)
    {
        std::cout << compressed[j];
    }
    std::cout << std::endl
         << std::endl;
}
