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
#include "hls_stream.h"
#include <ap_int.h>
#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <string>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lz4_compress.hpp"
#include "lz_compress.hpp"
#include "lz_optional.hpp"

#include "axi_to_stream.hpp"
#include "stream_to_axi.hpp"

#define MULTIPLE_BYTES 1

#define MAX_LIT_COUNT 4096
#define NUM_PARALLEL_BLOCK 1
#define LZ_MAX_OFFSET_LIMIT 65536
#define OFFSET_WINDOW (64 * 1024)
#define MAX_MATCH_LEN 255
#define MATCH_LEN 6

typedef ap_uint<32> compressd_dt;
typedef ap_uint<64> lz4_compressd_dt;
typedef ap_uint<8> uintV_t;

int const c_minMatch = 4;
int const c_matchLevel = 6;
int const c_minOffset = 1;
int const c_lz4MaxLiteralCount = MAX_LIT_COUNT;

void lz4CompressEngineRun(hls::stream<uintV_t>& inStream,
                          hls::stream<uintV_t>& lz4Out,
                          hls::stream<bool>& lz4Out_eos,
                          hls::stream<uint32_t>& lz4OutSize,
                          uint32_t max_lit_limit[NUM_PARALLEL_BLOCK],
                          uint32_t input_size,
                          uint32_t core_idx) {
    hls::stream<compressd_dt> compressdStream("compressdStream");
    hls::stream<xf::compression::compressd_dt> bestMatchStream("bestMatchStream");
    hls::stream<compressd_dt> boosterStream("boosterStream");

#pragma HLS STREAM variable = compressdStream depth = 8
#pragma HLS STREAM variable = bestMatchStream depth = 8
#pragma HLS STREAM variable = boosterStream depth = 8

#pragma HLS RESOURCE variable = compressdStream core = FIFO_SRL
#pragma HLS RESOURCE variable = boosterStream core = FIFO_SRL

    xf::compression::lzCompress<MATCH_LEN, c_minMatch, LZ_MAX_OFFSET_LIMIT, c_matchLevel, c_minOffset>(
        inStream, compressdStream, input_size);
    xf::compression::lzBestMatchFilter<MATCH_LEN, OFFSET_WINDOW>(compressdStream, bestMatchStream, input_size);
    xf::compression::lzBooster<MAX_MATCH_LEN>(bestMatchStream, boosterStream, input_size);
    xf::compression::lz4Compress<MAX_LIT_COUNT, NUM_PARALLEL_BLOCK>(boosterStream, lz4Out, max_lit_limit, input_size,
                                                                lz4Out_eos, lz4OutSize, core_idx);
}

static void read_input_xilinx(uint8_t* input, hls::stream<uintV_t>& inStream, unsigned long input_size) 
{
    hls::stream<bool> inStream_eos("compressIn_eos");
    xf::common::utils_hw::axiToStream<8>((ap_uint<8> *)input, input_size, inStream, inStream_eos);
    while(!inStream_eos.read()) {
    }
}

static void write_output_xilinx(uint8_t* output, hls::stream<uintV_t>& outStream, hls::stream<bool>& outStream_eos, uint32_t input_size, uint32_t new_size)
{
    *((uint32_t *)output) = input_size;
    uint8_t* start = output + 4;
    xf::common::utils_hw::streamToAxi<8>((ap_uint<8> *)start, outStream, outStream_eos);
    uintV_t o = outStream.read();
}

static void read_input(uint8_t* input, hls::stream<uintV_t>& inStream, unsigned long input_size) 
{
    // std::cout << "read_input: " << std::to_string(input_size) << std::endl;
    for (int i = 0; i < input_size; i++) {
        inStream << input[i];
    }
}

static void write_output(uint8_t* output, hls::stream<uintV_t>& outStream, hls::stream<bool>& outStream_eos,  uint32_t input_size, uint32_t new_size)
{
    // std::cout << "write_output input_size: " << std::to_string(input_size) << " new_size: " << std::to_string(new_size) << std::endl;
    *((uint32_t *)output) = input_size;
    uint32_t outCnt = 0;
    for (bool outEoS = outStream_eos.read(); outEoS == 0; outEoS = outStream_eos.read()) {
        // reading value from output stream
        uintV_t o = outStream.read();

        // writing to output array
        if (outCnt + MULTIPLE_BYTES < new_size) {
            ((uintV_t *)output)[4 + outCnt / MULTIPLE_BYTES] = o;
            outCnt += MULTIPLE_BYTES;
        } else {
            ((uintV_t *)output)[4 + outCnt / MULTIPLE_BYTES] = o;
            outCnt = new_size;
        }
    }
    // read outStream to avoid this --- WARNING: Hls::stream 'compressOut' contains leftover data, which may result in RTL simulation hanging.
    uintV_t o = outStream.read();

    // // the first 4 bytes are the size
    // for (int i = 0; i < new_size; i++) {
    //     output[4+i] = outStream.read();
    //     eos_flag = outStream_eos.read();
    // }
}

uint32_t lz4_compress(uint8_t *input, uint8_t *output, unsigned long input_size)
{
#pragma HLS INTERFACE m_axi port=input offset=off bundle=gmem depth=1048576
#pragma HLS INTERFACE m_axi port=output offset=off bundle=gmem depth=1048576
#pragma HLS interface ap_ctrl_hs port=return
    // std::cout << "lz4_compress" << std::endl;
    // offset output

    // NOOP... just copy in to out
    memcpy(output, (const uint8_t*)input, input_size);
	return input_size;

//     hls::stream<uintV_t> inStream("compressIn");
//     hls::stream<uintV_t> outStream("compressOut");
//     hls::stream<bool> outStream_eos("compressOut_eos");
//     hls::stream<uint32_t> outStream_size("compressOut_size");
//     uint32_t max_lit_limit[NUM_PARALLEL_BLOCK];
//     uint32_t core_idx;

// #pragma HLS dataflow
//     // read data from input array to input stream inStream
//     read_input(input, inStream, input_size);
//     // compression
//     lz4CompressEngineRun(inStream, outStream, outStream_eos, outStream_size, max_lit_limit, input_size, 0);
//     // write data from stream to output array
//     uint32_t new_size = outStream_size.read();
//     // std::cout << "new_size: " << std::to_string(new_size) << std::endl;
//     write_output(output, outStream, outStream_eos, input_size, new_size);

//     // output[0] = '1';
//     // output[1] = '1';
//     // output[2] = '2';
//     // output[3] = '2';
//     // output[4] = '3';
//     // output[5] = '3';
//     // input[0] = '3';
//     // input[1] = '3';
//     // input[2] = '2';
//     // input[3] = '2';
//     // input[4] = '1';
//     // input[5] = '1';
//     // the header of snappy
//     uint32_t array_size = new_size + 4;
//     return array_size;
}

uint32_t read_file(char *filename, char *array)
{
    std::fstream inputFile;
    inputFile.open(filename, std::fstream::binary | std::fstream::in);
    if (!inputFile.is_open()) {
        std::cout << "Cannot open the input file!!" << std::endl;
        exit(0);
    }
    inputFile.seekg(0, std::ios::end); // reaching to end of file
    uint32_t file_length = (uint32_t)inputFile.tellg();
    inputFile.seekg(0, std::ios::beg);
    for (int i = 0; i < file_length; i += MULTIPLE_BYTES) {
        uintV_t x;
        inputFile.read((char*)&x, MULTIPLE_BYTES);
        ((uintV_t *)array)[i] = x;
    }
    inputFile.close();
    // // for testing
    // std::cout << "read_file: " << std::to_string(file_length) << std::endl;
    // for (int i = 0; i < file_length; i++) {
    //     std::cout << array[i];
    // }
    // std::cout << std::endl;
    return file_length;
}

void write_file(char *filename, char *array, uint32_t size)
{
    // std::cout << "write_file " << std::to_string(size) << std::endl;
    std::ofstream outFile;
    outFile.open(filename, std::fstream::binary | std::fstream::out);
    uint32_t outCnt = 0;
    for (outCnt = 0; outCnt + MULTIPLE_BYTES < size; outCnt += MULTIPLE_BYTES) {
        uintV_t o = ((uintV_t *)array)[outCnt / MULTIPLE_BYTES];
        outFile.write((char*)&o, MULTIPLE_BYTES);
    }
    uintV_t o = ((uintV_t *)array)[outCnt / MULTIPLE_BYTES];
    outFile.write((char*)&o, size - outCnt);
    outFile.close();
}

int main(int argc, char* argv[]) {
    char *buf = (char *)malloc(4 * 1024 * 1024); // 4MB buffer
    char *input = buf;
    char *output = buf + 1024 * 1024;

    for (int i = 0; i < 1024; i++) {
        input[i] = 'p';
    }
    u_int32_t input_size = 1024;
    // compressed file to array
    // uint32_t input_size = read_file(argv[1], input);
    // DECOMPRESSION CALL
    uint32_t array_size = lz4_compress((uint8_t *)input, (uint8_t *)output, input_size);
    // std::cout << "array_size: " << std::to_string(array_size) << std::endl;
    // array to decompressed file
    // write_file(argv[2], output, array_size);
    for (int i = 0; i < 1024; i++) {
        std::cout << output[i];
    }
    std::cout << std::endl;
}
