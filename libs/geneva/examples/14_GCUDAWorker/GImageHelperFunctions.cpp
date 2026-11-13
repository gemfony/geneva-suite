/**
* @file GImageHelperFunctions.cpp
 */

/********************************************************************************
 *
 * This file is part of the Geneva library collection. The following license
 * applies to this file:
 *
 * ------------------------------------------------------------------------------
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ------------------------------------------------------------------------------
 *
 * Note that other files in the Geneva library collection may use a different
 * license. Please see the licensing information in each file.
 *
 ********************************************************************************
 *
 * See the NOTICE file in the top-level directory of the Geneva library
 * collection for a list of contributors and copyright information.
 *
 ********************************************************************************/

#include "GImageHelperFunctions.hpp"

#include <cstring>
#include <ranges>


namespace Gem::Common {
/******************************************************************************/
/**
      * Loads a PNG file from disk and outputs an RGB array (unsigned char, 8 bits/channel),
      * storing it in a std::vector<unsigned char>. Also outputs the image dimensions.
      *
      * @param filename  The name of the PNG file to be loaded (as a std::string)
      * @param outData   A reference to a std::vector<unsigned char> to hold the raw RGB data
      * @param outWidth  A reference to an integer for the width of the image
      * @param outHeight A reference to an integer for the height of the image
      * @return A boolean indicating whether the loading was successful
      */
// NOLINTNEXTLINE(readability-function-size) -- one coherent libpng read kernel; each step depends on png_ptr/info_ptr state opened above it, with a matching cleanup at every early-return, so splitting would scatter tightly coupled cleanup logic
bool loadPngToRGB(
    const std::string &filename,
    std::vector<unsigned char> &outData,
    int &outWidth,
    int &outHeight
) {
    // Open the file using C stdio
    FILE *fp = fopen(filename.c_str(), "rb");
    if(!fp) {
        std::cerr << "Error opening the PNG file: " << filename << '\n';
        return false;
    }

    // Create the main libpng structures
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if(!png_ptr) {
        fclose(fp);
        return false;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if(!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        fclose(fp);
        return false;
    }

    // Error handling via setjmp/longjmp in libpng
    if(setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(fp);
        return false;
    }

    // Initialize libpng I/O
    png_init_io(png_ptr, fp);
    png_read_info(png_ptr, info_ptr);

    // Get image width/height and color/bit info
    const int width = png_get_image_width(png_ptr, info_ptr);
    const int height = png_get_image_height(png_ptr, info_ptr);
    const png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    const png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    // Convert to 8 bits per channel if necessary
    if(bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if(color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if(png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    }
    if(color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }
    // If RGBA, remove the alpha channel (we only want RGB)
    if(color_type == PNG_COLOR_TYPE_RGBA) {
        png_set_strip_alpha(png_ptr);
    }

    // Update the info struct after transformations
    png_read_update_info(png_ptr, info_ptr);

    // Number of bytes per row in the read data. Kept as the size_t libpng returns: narrowing it to
    // int would truncate for a very wide image, and the row-buffer size below would then be
    // computed as an int product that can overflow before it reaches malloc().
    const std::size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);

    // Allocate temporary memory for reading row data
    png_byte *image_data = static_cast<png_byte *>(malloc(rowbytes * height));
    if(!image_data) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(fp);
        return false;
    }

    // Allocate array of row-pointers
    auto *const row_pointers = static_cast<png_bytep *>(malloc(sizeof(png_bytep) * height));
    if(!row_pointers) {
        free(image_data);
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(fp);
        return false;
    }

    // Set each row pointer to the appropriate offset in image_data
    for(int y = 0; y < height; y++) {
        row_pointers[y] = image_data + y * rowbytes;
    }

    // Read the image data
    png_read_image(png_ptr, row_pointers);
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    fclose(fp);
    free(row_pointers);

    // We want pure 3-channel RGB data (24 bits total).
    // If rowbytes included an alpha channel, it was removed above.
    unsigned char *finalData =
        static_cast<unsigned char *>(malloc(width * height * 3 * sizeof(unsigned char)));
    if(!finalData) {
        free(image_data);
        return false;
    }

    // Copy (width * 3) bytes per row
    for(int y = 0; y < height; y++) {
        std::memcpy(
            finalData + (y * width * 3),
            image_data + (y * rowbytes),
            static_cast<size_t>(width) * 3
        );
    }
    free(image_data);

    // Transfer finalData to outData (the std::vector)
    outData.resize(static_cast<size_t>(width) * height * 3);
    std::memcpy(outData.data(), finalData, static_cast<size_t>(width) * height * 3);

    free(finalData);

    // Set the width/height references
    outWidth = width;
    outHeight = height;

    return true;
}

/******************************************************************************/
/**
     * @brief Writes a raw 8-bit RGB array (3 bytes per pixel) to a PNG file.
     *
     * This function writes the contents of an RGB buffer to a PNG file using libpng.
     * The provided vector must contain at least (width * height * 3) bytes, representing
     * the RGB channels in row-major order (row by row).
     *
     * @param filename  The path to the output PNG file.
     * @param rgbData   A vector of unsigned char containing the RGB data.
     * @param width     The width of the image in pixels.
     * @param height    The height of the image in pixels.
     * @return True if the PNG file was created successfully, otherwise false.
     */
bool writeRGBtoPNG(
    const std::string &filename,
    const std::vector<unsigned char> &rgbData,
    int width,
    int height
) {
    // Basic sanity check: ensure rgbData has enough bytes
    if(rgbData.size() < static_cast<size_t>(width) * height * 3) {
        fprintf(stderr, "Error: rgbData is too small for the given width/height.\n");
        return false;
    }

    // Open the file in binary write mode
    FILE *fp = fopen(filename.c_str(), "wb");
    if(!fp) {
        fprintf(stderr, "Error creating PNG file: %s\n", filename.c_str());
        return false;
    }

    // Create PNG write struct
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if(!png_ptr) {
        fclose(fp);
        return false;
    }

    // Create PNG info struct
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if(!info_ptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        fclose(fp);
        return false;
    }

    // Error handling for libpng
    if(setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return false;
    }

    // Initialize PNG I/O
    png_init_io(png_ptr, fp);

    // Set header info (8 bits per channel, RGB)
    png_set_IHDR(
        png_ptr,
        info_ptr,
        static_cast<png_uint_32>(width),
        static_cast<png_uint_32>(height),
        8,                  // bit depth
        PNG_COLOR_TYPE_RGB, // color type
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );

    // Write the header info
    png_write_info(png_ptr, info_ptr);

    // Write each row
    for(int y = 0; y < height; ++y) {
        png_bytep row_ptr = const_cast<png_bytep>(&rgbData[static_cast<size_t>(y) * width * 3]);
        png_write_row(png_ptr, row_ptr);
    }

    // Finalize writing
    png_write_end(png_ptr, info_ptr);

    // Cleanup
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);

    return true;
}

/**
     * Transfers an image to a local data structure in RGB format. This
     * function also extracts the image dimensions. It may be called concurrently from
     * different threads. Calling it with a different name from previous calls will not have
     * an effect.
     *
     * @param fileName The name of the image on disc from where the image shall be loaded
     * @param imageData_vec The vector into which the RGB data shall be stored
     * @param width The width of the disc image
     * @param height The height of the disc image
     * @return A boolean indicating whether loading of the image was successful
     */
bool loadImageToRGB(
    const std::string &fileName,
    std::vector<unsigned char> &imageData_vec,
    int &width,
    int &height
) {
    static std::mutex image_mutex;
    static std::vector<unsigned char> l_imageData_vec;
    static std::string l_fileName{};
    static bool first{true};
    static int l_width{0};
    static int l_height{0};

    // Serialize the loading and transfer of images
    std::lock_guard<std::mutex> const lock(image_mutex);

    // Loading from disc shall be done but once
    if(first) {
        first = false;
        l_fileName = fileName;

        // Identify the suffix of the filename (trailing characters after the last dot
        char const delimiter = '.';
        std::string suffix{};
        std::size_t const pos = l_fileName.find_last_of(delimiter);
        if(pos == std::string::npos || pos == l_fileName.length() - 1) {
            // No delimiter found, or delimiter is the last character
            suffix = "none";
        }
        else {
            suffix = l_fileName.substr(pos + 1);
        }

        if(suffix == "PNG" || suffix == "png") {
            // Do the actual loading
            if(not Common::loadPngToRGB(l_fileName, l_imageData_vec, l_width, l_height)) {
                std::cout << "Loading PNG image " << l_fileName << " from disc into RGB failed"
                          << '\n';
                return false;
            }
                            std::cout << "Successfully loaded PNG image " << l_fileName << " from disc into RGB"
                          << '\n';
           
        }
    }
    else {
        // Complain if the function was called with a different filename from the first time
        if(l_fileName != fileName) {
            std::cout << "Common::loadImageToRGB: Error. fileName (" << fileName
                      << ") and l_fileName (" << l_fileName << ") differ" << '\n';
            return false;
        }
    }

    // Now we know that we already have a valid data vector and
    // can transfer image data. This way we prevent repeated loading
    // of the same file from disc
    imageData_vec = l_imageData_vec;

    // Also transfer width and height
    width = l_width;
    height = l_height;

    return true;
}

/**
     * Transfers an image to a local data structure in float format. This
     * function also extracts the image dimensions. It may be called concurrently from
     * different threads. Calling it with a different name from previous calls will not have
     * an effect.
     *
     * @param fileName The name of the image on disc from where the image shall be loaded
     * @param imageData_f_vec The vector into which the RGB data shall be stored in float format
     * @param width The width of the disc image
     * @param height The height of the disc image
     * @return A boolean indicating whether loading of the image was successful
     */
bool loadImageToFloat(
    const std::string &fileName,
    std::vector<float> &imageData_f_vec,
    int &width,
    int &height
) {
    // Load the image in RGB format
    std::vector<unsigned char> imageData_vec;
    if(not loadImageToRGB(fileName, imageData_vec, width, height)) {
        return false;
    }

    const std::size_t channel_size = imageData_vec.size();
    if(channel_size != width * height * 3) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In loadImageToFloat: Error!" << '\n'
            << "Invalid dimensions " << width << " / " << height << '\n'
        );
    }

    // Transfer the channels to floats in the range 0..1
    imageData_f_vec = imageData_vec
        | std::views::transform([](auto c) { return static_cast<float>(c) / 255.0f; })
        | std::ranges::to<std::vector<float>>();

    return true;
}

/**
     * Writes an image in RGB format to disc. Note that this function will
     * become more useful when further graphics formats are supported.
     *
     * @param fileName The name of the file to which data should be written
     * @param imageData_vec The RGB-data to be copied to disc
     * @param width The width of the target image
     * @param height The height of the target image
     * @return A boolean indicating whether saving was successful
     */
bool saveRGBImageToFile(
    const std::string &fileName,
    const std::vector<unsigned char> &imageData_vec,
    const int width,
    const int height
) {
    // Identify the suffix of the filename (trailing characters after the last dot
    constexpr char delimiter = '.';
    std::string suffix{};
    std::size_t const pos = fileName.find_last_of(delimiter);
    if(pos == std::string::npos || pos == fileName.length() - 1) {
        // No delimiter found, or delimiter is the last character
        suffix = "none";
    }
    else {
        suffix = fileName.substr(pos + 1);
    }

    if(suffix == "PNG" || suffix == "png") {
        return writeRGBtoPNG(fileName, imageData_vec, width, height);
    }

    return false;
}

/**
     * Writes an image in float format to disc. Note that this function will
     * become more useful when further graphics formats are supported.
     *
     * @param fileName The name of the file to which data should be written
     * @param imageData_f_vec The float-data to be copied to disc
     * @param width The width of the target image
     * @param height The height of the target image
     * @return A boolean indicating whether saving was successful
     */
bool saveFloatImageToFile(
    const std::string &fileName,
    const std::vector<float> &imageData_f_vec,
    const int width,
    const int height
) {
    // Check the dimensions
    if(imageData_f_vec.size() != width * height * 3) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In saveFloatImageToFile: Error!" << '\n'
            << "Invalid dimensions: " << "width = " << width << " / " << "height = " << height
            << " / width * height * 3 = " << width * height * 3
            << " / imageData_f_vec.size() = " << imageData_f_vec.size() << '\n'
        );
    }

    // Convert the float-vector to RGB (clamp each value to [0, 1] to avoid out-of-range issues)
    const auto imageData_vec = imageData_f_vec
        | std::views::transform([](float channel_value) {
              return static_cast<unsigned char>(
                  std::round(std::clamp(channel_value, 0.0f, 1.0f) * 255.0f));
          })
        | std::ranges::to<std::vector<unsigned char>>();

    return saveRGBImageToFile(fileName, imageData_vec, width, height);
}
} /* namespace Gem::Common */
