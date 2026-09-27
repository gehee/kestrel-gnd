
#pragma once
//
// Created by gaeta on 2024-06-20.
//

#ifndef FPVUE_PRINT_UTIL_H
#define FPVUE_PRINT_UTIL_H


#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <iomanip>


inline  void printHexData(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        // Print each byte in hex, with 2 digits and leading zeros
        std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
    }
    std::cout << std::dec << std::endl;  // Switch back to decimal formatting
}

inline void print_hex(const char* title, const uint8_t* data, size_t size) {
  printf("\n-----------------%s---------------------\n", title);
  for (size_t i = 0; i < size; ++i) {
      // Check for 4-byte NALU start code
      if (i + 3 < size &&
          data[i] == 0x00 &&
          data[i + 1] == 0x00 &&
          data[i + 2] == 0x00 &&
          data[i + 3] == 0x01) {
          // New NALU — start new line
          if (i != 0) printf("\n");
      }
      printf("%02X ", data[i]);
  }
  printf("\n---------------------------------------\n");
  // exit(1);
}


#endif //FPVUE_PRINT_UTIL_H