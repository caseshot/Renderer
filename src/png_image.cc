#include <cstdint>
#include <iostream>
#include <cstring>
#include <fstream>
#include <memory>
#include <zconf.h>
#include <zlib.h>
#include "png_image.hh"


static const std::uint64_t kPngSign = 0x89504e470d0a1a0a;

// most of computer cpu use little endian, 
// PNG file use big endian, so we need reverse endianness
inline constexpr std::uint64_t reverseEndian(std::uint64_t val) {
    const std::uint64_t fmt1 = 0x00ff00ff00ff00ff;
    const std::uint64_t fmt2 = 0x0000ffff0000ffff;
    const std::uint64_t fmt3 = 0x00000000ffffffff;

    val = ((val & fmt1) << 8) | ((val & (fmt1 << 8)) >> 8);
    val = ((val & fmt2) << 16) | ((val & (fmt2 << 16)) >> 16);
    val = ((val & fmt3) << 32) | ((val & (fmt3 << 32)) >> 32);
    return val;
}

inline constexpr std::uint32_t reverseEndian(std::uint32_t val) {
    const std::uint32_t fmt1 = 0x00ff00ff;
    const std::uint32_t fmt2 = 0x0000ffff;

    val = ((val & fmt1) << 8) | ((val & (fmt1 << 8)) >> 8);
    val = ((val & fmt2) << 16) | ((val & (fmt2 << 16)) >> 16);
    return val;
}

inline std::uint32_t calCRC32(PNGDataBlockType type, const std::uint8_t* data, size_t length) {
    std::uint8_t buf[4 + length];
    std::uint32_t type_val = reverseEndian((std::uint32_t) type);
    memcpy(buf, (const char*) &type_val, 4);
    memcpy(buf + 4, data, length);
    return crc32(0, buf, 4 + length);
}

PNGImage::PNGImage(): header_(), data_() {}

PNGImage::PNGImage(const PNGImage& image): 
        header_(image.header_), 
        data_(new std::uint8_t[sizeof(std::uint8_t) * image.size()]) {
    memcpy(data_.get(), image.data_.get(), sizeof(std::uint8_t) * image.size());
}

bool PNGImage::readPNG(const std::string& png_file_name) {
    std::ifstream file(png_file_name, std::ios::in | std::ios::binary);

    std::shared_ptr<std::uint8_t[]> compressed_data;
    std::uint32_t data_index = 0;

    if(!file.is_open()) 
        goto failed;

    std::uint64_t sign;
    file.read((char*) &sign, 8);
    if(reverseEndian(sign) != kPngSign)
        goto failed;

    std::uint32_t length;
    std::uint32_t block_type;
    std::uint32_t crc;

    while(true) {
        file.read((char*) &length, 4);
        file.read((char*) &block_type, 4);
        length = reverseEndian(length);
        block_type = reverseEndian(block_type);
        
        if(block_type == IEND) {
            int bpp = (header_.color_type == TRUE_COLOR ? 3 : 4);
            std::uint32_t data_width = header_.width * bpp;
            std::uint32_t data_height = header_.height;
            uLongf len = (data_width + 1) * data_height;
            std::shared_ptr<std::uint8_t[]> filtered_data(new std::uint8_t[len]);
            
            int loop_count = 0;
            do {
                int res = uncompress(filtered_data.get(), &len, compressed_data.get(), data_index);
                if(res == Z_OK) {
                    data_.reset(new std::uint8_t[data_width * data_height]);
                    for(std::uint32_t i = 0; i < data_height; i++) {
                        std::uint32_t data_begin = i * data_width;
                        std::uint32_t filter_begin = i * (data_width + 1);
                        memcpy(data_.get() + data_begin, filtered_data.get() + filter_begin + 1, data_width);
                        switch(filtered_data[filter_begin]) {
                            case 0x00:
                                break;
                            case 0x01: {
                                for(std::uint32_t k = data_begin + bpp; k < data_begin + data_width; k++)
                                    data_[k] = data_[k] + data_[k - bpp];
                                break;
                            }
                            case 0x02: {
                                if(i == 0) goto failed;
                                for(std::uint32_t k = data_begin; k < data_begin + data_width; k++)
                                    data_[k] = data_[k] + data_[k - data_width];
                                break;
                            }
                            case 0x03: {
                                if(i == 0) goto failed;
                                for(std::uint32_t k = data_begin + bpp; k < data_begin + data_width; k++)
                                    data_[k] = data_[k] + ((int) data_[k - bpp] + (int) data_[k - data_width]) / 2;
                                break;
                            }
                            case 0x04: {
                                if(i == 0) goto failed;
                                for(std::uint32_t k = data_begin + bpp; k < data_begin + data_width; k++) {
                                    int v = abs((int) data_[k - bpp] - (int) data_[k - data_width - bpp]);
                                    int h = abs((int) data_[k - data_width] - (int) data_[k - data_width - bpp]);
                                    data_[k] = data_[k] + (v < h ? data_[k - data_width] : data_[k - bpp]);
                                }
                                break;
                            }
                            default:
                                goto failed;
                        }
                    }
                    break;
                }else if(res == Z_MEM_ERROR) {
                    if(++loop_count < 10)
                        continue;
                    else
                        goto failed;
                }else {
                    goto failed;
                }
            }while(true);
            file.close();
            return true;
        }else if(block_type == IHDR) {
            file.read((char*) &header_, length);
            file.read((char*) &crc, 4);

            if(reverseEndian(crc) != calCRC32(IHDR, (std::uint8_t*) &header_, length))
                goto failed;

            if(header_.color_type != TRUE_COLOR && header_.color_type != TRUE_COLOR_ALPHA)
                goto failed;

            header_.width = reverseEndian(header_.width);
            header_.height = reverseEndian(header_.height);
            compressed_data.reset(new std::uint8_t[(header_.width * 4 + 1) * header_.height]);
        }else if(block_type == IDAT){
            if(!compressed_data) goto failed;
            std::shared_ptr<std::uint8_t[]> buf(new std::uint8_t[length]);
            file.read((char*) buf.get(), length);
            file.read((char*) &crc, 4);

            if(reverseEndian(crc) != calCRC32(IDAT, (std::uint8_t*) buf.get(), length))
                goto failed;

            memcpy(compressed_data.get() + data_index, buf.get(), length);
            data_index += length;

            
        }else { 
            file.seekg(length + 4, std::ios::cur);
        }
    }

failed:
    file.close();
    return false;
}