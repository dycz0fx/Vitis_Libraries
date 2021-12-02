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
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <string>

#include "lz4_decompress.hpp"
#include "lz_decompress.hpp"
#include "lz_optional.hpp"

#include "axi_to_stream.hpp"
#include "stream_to_axi.hpp"

#define MULTIPLE_BYTES 1

#define MAX_OFFSET 65536
#define HISTORY_SIZE MAX_OFFSET

typedef ap_uint<32> compressd_dt;
typedef ap_uint<8> uintV_t;

void lz4DecompressEngineRun(hls::stream<uintV_t>& inStream,
                            hls::stream<uintV_t>& outStream,
                            const uint32_t _input_size,
                            const uint32_t _output_size) {
    uint32_t input_size = _input_size;
    uint32_t output_size = _output_size;
    hls::stream<compressd_dt> decompressd_stream("decompressd_stream");
#pragma HLS STREAM variable = decompressd_stream depth = 8
#pragma HLS RESOURCE variable = decompressd_stream core = FIFO_SRL

#pragma HLS dataflow
    xf::compression::lz4Decompress(inStream, decompressd_stream, input_size);
    xf::compression::lzDecompress<HISTORY_SIZE>(decompressd_stream, outStream, output_size);
}

static void read_input(uint8_t* input, hls::stream<uintV_t>& inStream, uint32_t input_size) 
{
    // std::cout << "read_input: " << std::to_string(input_size) << std::endl;
    for (uint32_t i = 4; i < input_size; i += MULTIPLE_BYTES) {
        uintV_t x = ((uintV_t *)input)[i];
        inStream << x;
    }
}

static void read_input_xilinx(uint8_t* input, hls::stream<uintV_t>& inStream, uint32_t input_size)
{
    // std::cout << "read_input: " << std::to_string(input_size) << std::endl;
    hls::stream<bool> inStream_eos("compressIn_eos");
    int size = input_size / MULTIPLE_BYTES;
    if (input_size % MULTIPLE_BYTES != 0)
        size++;
    uint8_t* start = input + 4;
    xf::common::utils_hw::axiToStream<MULTIPLE_BYTES * 8>((uintV_t *)start, size, inStream, inStream_eos);
    while(!inStream_eos.read()) {
    }
}

static void write_output(uint8_t* output, hls::stream<uintV_t>& outStream, uint32_t output_size)
{
    // std::cout << "write_output" << std::endl;
    for (uint32_t i = 0; i < output_size; i++) {
        output[i] = outStream.read();
    }
}

uint32_t lz4_decompress(uint8_t *base, unsigned long input_offset, unsigned long output_offset, uint32_t input_size)
{
#pragma HLS INTERFACE m_axi port=base offset=off bundle=gmem depth=4194304
#pragma HLS interface ap_ctrl_hs port=return
    // std::cout << "lz4_decompress" << std::endl;
    // offset output
    uint8_t *input = base + input_offset;
	uint8_t *output = base + output_offset;

    hls::stream<uintV_t> inStream("inStream");
    hls::stream<uintV_t> outStream("decompressOut");

    uint32_t output_size = ((uint32_t *)input)[0];
    // read data from input array to input stream bytestr_in
    read_input(input, inStream, input_size);
    // read_input_xilinx(input, inStream, input_size);
    // decompression
    lz4DecompressEngineRun(inStream, outStream, input_size - 4, output_size);
    // write data from stream to output array
    write_output(output, outStream, output_size);
    return output_size;
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
    // std::cout << "read_file: " << std::endl;
    // for (int i = 0; i < file_length; i++) {
    //     std::cout << array[i];
    // }
    // std::cout << std::endl;
    return file_length;
}

void write_file(char *filename, char *array, uint32_t size)
{
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

bool verify(char *compressed_file, char *original_file) 
{
    bool pass = true;
    std::ifstream decompressedFile;
    decompressedFile.open(compressed_file, std::ofstream::binary | std::ofstream::in);
    std::ifstream originalFile;
    originalFile.open(original_file, std::ofstream::binary | std::ofstream::in);
    if (!originalFile.is_open()) {
        std::cout << "Cannot open the original file!!" << std::endl;
        exit(0);
    }
    decompressedFile.seekg(0, std::ios::end); // reaching to end of file
    uint32_t decompressed_length = (uint32_t)decompressedFile.tellg();
    decompressedFile.seekg(0, std::ios::beg);
    originalFile.seekg(0, std::ios::end); // reaching to end of file
    uint32_t original_length = (uint32_t)originalFile.tellg();
    if (decompressed_length != original_length) {
        // std::cout << "length wrong!!! original = " << std::to_string(original_length) << " decompressed = " << std::to_string(decompressed_length) << std::endl;
        pass = false;
    }
    originalFile.seekg(0, std::ios::beg);
    uintV_t g;
    uintV_t o;
    for (int i = 0; i < original_length; i += MULTIPLE_BYTES) {
        originalFile.read((char*)&g, MULTIPLE_BYTES);
        decompressedFile.read((char*)&o, MULTIPLE_BYTES);
        if (o != g) {
            pass = false;
            std::cout << "Expected=" << std::hex << o << " got=" << g << std::endl;
            std::cout << "-----TEST FAILED: The input file and the file after "
                      << "decompression are not similar!-----" << std::endl;
        }
    }
    decompressedFile.close();
    originalFile.close();
    if (pass) {
        std::cout << "============== TEST PASSED ==============" << std::endl;
    } else {
        std::cout << "============== TEST FAILED ==============" << std::endl;
    }
    return pass;
}

int main(int argc, char* argv[]) {
    char *buf = (char *)malloc(3 * 1024 * 1024); // 3MB buffer
    char *input = buf;
    char *output = buf + 1024 * 1024;

    // compressed file to array
    uint32_t input_size = read_file(argv[1], input);
    // DECOMPRESSION CALL
    uint32_t new_size = lz4_decompress((uint8_t *)buf, input - buf, output - buf, input_size);
    // std::cout << "new_size: " << std::to_string(new_size) << std::endl;
    // array to decompressed file
    write_file(argv[2], output, new_size);

    verify(argv[2], argv[3]);
}
