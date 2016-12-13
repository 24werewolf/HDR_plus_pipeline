#include "finish.h"

#include "Halide.h"
#include "halide_image_io.h"
#include "util.h"

using namespace Halide;
using namespace Halide::ConciseCasts;

Func black_white_point(Func input, const BlackPoint bp, const BlackPoint wp) {

    Func output("black_white_point_output");

    Var x, y;

    float white_factor = 65535.f / (wp - bp);

    output(x, y) = u16_sat((i32(input(x, y)) - bp) * white_factor);

    return output;
}


Func white_balance(Func input, int width, int height, const WhiteBalance &wb) {

    Func output("white_balance_output");

    Var x, y;
    RDom r(0, width / 2, 0, height / 2);            // reduction over half of image

    output(x, y) = u16(0);

    output(r.x * 2    , r.y * 2    ) = u16_sat(wb.r  * f32(input(r.x * 2    , r.y * 2    )));   // red
    output(r.x * 2 + 1, r.y * 2    ) = u16_sat(wb.g0 * f32(input(r.x * 2 + 1, r.y * 2    )));   // green 0
    output(r.x * 2    , r.y * 2 + 1) = u16_sat(wb.g1 * f32(input(r.x * 2    , r.y * 2 + 1)));   // green 1
    output(r.x * 2 + 1, r.y * 2 + 1) = u16_sat(wb.b  * f32(input(r.x * 2 + 1, r.y * 2 + 1)));   // blue

    ///////////////////////////////////////////////////////////////////////////
    // schedule
    ///////////////////////////////////////////////////////////////////////////

    output.compute_root().parallel(y).vectorize(x, 16);

    output.update(0).parallel(r.y);
    output.update(1).parallel(r.y);
    output.update(2).parallel(r.y);
    output.update(3).parallel(r.y);

    return output;
}

Func demosaic(Func input, int width, int height) {

    // Technique from: https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/Demosaicing_ICASSP04.pdf

    // assumes RG/GB Bayer pattern

    Func f0("demosaic_f0");             // G at R locations; G at B locations
    Func f1("demosaic_f1");             // R at green in R row, B column; B at green in B row, R column
    Func f2("demosaic_f2");             // R at green in B row, R column; B at green in R row, B column
    Func f3("demosaic_f3");             // R at blue in B row, B column; B at red in R row, R column

    Func d0("demosaic_0");
    Func d1("demosaic_1");
    Func d2("demosaic_2");
    Func d3("demosaic_3");

    Func output("demosaic_output");

    Var x, y, c;
    RDom r0(-2, 5, -2, 5);                          // reduction over weights in filter
    RDom r1(0, width / 2, 0, height / 2);           // reduction over half of image

    // mirror input image with overlapping edges to keep mosaic pattern consistency

    Func input_mirror = BoundaryConditions::mirror_interior(input, 0, width, 0, height);

    // demosaic filters

    f0(x,y) = 0;
    f1(x,y) = 0;
    f2(x,y) = 0;
    f3(x,y) = 0;

    int f0_sum = 8;
    int f1_sum = 16;
    int f2_sum = 16;
    int f3_sum = 16;

                                    f0(0, -2) = -1;
                                    f0(0, -1) =  2;
    f0(-2, 0) = -1; f0(-1, 0) = 2;  f0(0,  0) =  4; f0(1, 0) = 2;   f0(2, 0) = -1;
                                    f0(0,  1) =  2;
                                    f0(0,  2) = -1;

                                    f1(0, -2) =  1;
                    f1(-1,-1) = -2;                 f1(1, -1) = -2;
    f1(-2, 0) = -2; f1(-1, 0) =  8; f1(0,  0) = 10; f1(1,  0) =  8; f1(2, 0) = -2;
                    f1(-1, 1) = -2;                 f1(1,  1) = -2;
                                    f1(0,  2) =  1;

                                    f2(0, -2) = -2;
                    f2(-1,-1) = -2; f2(0, -1) =  8; f2(1, -1) = -2;
    f2(-2, 0) = 1;                  f2(0,  0) = 10;                 f2(2, 0) = 1;
                    f2(-1, 1) = -2; f2(0,  1) =  8; f2(1,  1) = -2;
                                    f2(0,  2) = -2;

                                    f3(0, -2) = -3;
                    f3(-1,-1) = 4;                  f3(1, -1) = 4;
    f3(-2, 0) = -3;                 f3(0,  0) = 12;                 f3(2, 0) = -2;
                    f3(-1, 1) = 4;                  f3(1,  1) = 4;
                                    f3(0,  2) = -3;

    // intermediate demosaic functions

    d0(x, y) = u16_sat(sum(i32(input_mirror(x + r0.x, y + r0.y)) * f0(r0.x, r0.y)) / f0_sum);
    d1(x, y) = u16_sat(sum(i32(input_mirror(x + r0.x, y + r0.y)) * f1(r0.x, r0.y)) / f1_sum);
    d2(x, y) = u16_sat(sum(i32(input_mirror(x + r0.x, y + r0.y)) * f2(r0.x, r0.y)) / f2_sum);
    d3(x, y) = u16_sat(sum(i32(input_mirror(x + r0.x, y + r0.y)) * f3(r0.x, r0.y)) / f3_sum);

    // resulting demosaicked function    

    output(x, y, c) = input(x, y);                                              // initialize each channel to input mosaicked image

    // red
    output(r1.x * 2 + 1, r1.y * 2,     0) = d1(r1.x * 2 + 1, r1.y * 2);         // R at green in R row, B column
    output(r1.x * 2,     r1.y * 2 + 1, 0) = d2(r1.x * 2,     r1.y * 2 + 1);     // R at green in B row, R column
    output(r1.x * 2 + 1, r1.y * 2 + 1, 0) = d3(r1.x * 2 + 1, r1.y * 2 + 1);     // R at blue in B row, B column

    // green
    output(r1.x * 2,     r1.y * 2,     1) = d0(r1.x * 2,     r1.y * 2);         // G at R locations
    output(r1.x * 2 + 1, r1.y * 2 + 1, 1) = d0(r1.x * 2 + 1, r1.y * 2 + 1);     // G at B locations

    // blue
    output(r1.x * 2,     r1.y * 2 + 1, 2) = d1(r1.x * 2,     r1.y * 2 + 1);     // B at green in B row, R column
    output(r1.x * 2 + 1, r1.y * 2,     2) = d2(r1.x * 2 + 1, r1.y * 2);         // B at green in R row, B column
    output(r1.x * 2,     r1.y * 2,     2) = d3(r1.x * 2,     r1.y * 2);         // B at red in R row, R column

    ///////////////////////////////////////////////////////////////////////////
    // schedule
    ///////////////////////////////////////////////////////////////////////////

    f0.compute_root().parallel(y).parallel(x);
    f1.compute_root().parallel(y).parallel(x);
    f2.compute_root().parallel(y).parallel(x);
    f3.compute_root().parallel(y).parallel(x);

    d0.compute_root().parallel(y).vectorize(x, 16);
    d1.compute_root().parallel(y).vectorize(x, 16);
    d2.compute_root().parallel(y).vectorize(x, 16);
    d3.compute_root().parallel(y).vectorize(x, 16);

    output.compute_root().parallel(y).vectorize(x, 16);

    output.update(0).parallel(r1.y);
    output.update(1).parallel(r1.y);
    output.update(2).parallel(r1.y);
    output.update(3).parallel(r1.y);
    output.update(4).parallel(r1.y);
    output.update(5).parallel(r1.y);
    output.update(6).parallel(r1.y);
    output.update(7).parallel(r1.y);

    return output;
}

Func combine(Func im1, Func im2, int width, int height, Func dist) {

    // exposure fusion as described by Mertens et al. modified to only use intensity metric
    // http://ntp-0.cs.ucl.ac.uk/staff/j.kautz/publications/exposure_fusion.pdf

    Func init_mask1("mask1_layer_0");
    Func init_mask2("mask2_layer_0");
    Func accumulator("combine_accumulator");
    Func output("combine_output");

    Var x, y;

    // mirror input images

    Func im1_mirror = BoundaryConditions::repeat_edge(im1, 0 , width, 0, height);
    Func im2_mirror = BoundaryConditions::repeat_edge(im2, 0 , width, 0, height);

    // initial blurred layers to compute laplacian pyramid

    Func unblurred1 = im1_mirror;
    Func unblurred2 = im2_mirror;

    Func blurred1 = gauss_7x7(im1_mirror, "img1_layer_0");
    Func blurred2 = gauss_7x7(im2_mirror, "img2_layer_0");

    Func laplace1, laplace2, mask1, mask2;

    // initial masks computed from input distribution function

    Expr weight1 = f32(dist(im1(x,y)));
    Expr weight2 = f32(dist(im2(x,y)));

    init_mask1(x, y) = weight1 / (weight1 + weight2);
    init_mask2(x, y) = 1.f - init_mask1(x, y);

    mask1 = init_mask1;
    mask2 = init_mask2;

    // blend frequency band of images with corresponding frequency band of weights; accumulate over frequency bands

    int num_layers = 3;

    accumulator(x, y) = i32(0);

    for (int layer = 1; layer < num_layers; layer++) {

        std::string prev_layer_str = std::to_string(layer - 1);
        std::string layer_str = std::to_string(layer);

        // previous laplace layer

        laplace1 = diff(unblurred1, blurred1, "laplace1_layer_" + prev_layer_str);
        laplace2 = diff(unblurred2, blurred2, "laplace2_layer_" + layer_str);

        // add previous frequency band

        accumulator(x, y) += i32(laplace1(x,y) * mask1(x,y)) + i32(laplace2(x,y) * mask2(x,y));

        // save previous gauss layer to produce current laplace layer

        unblurred1 = blurred1;
        unblurred2 = blurred2;

        // current gauss layer of images

        blurred1 = gauss_7x7(blurred1, "img1_layer_" + layer_str);
        blurred2 = gauss_7x7(blurred2, "img1_layer_" + layer_str);

        // current gauss layer of masks

        mask1 = gauss_7x7(mask1, "mask1_layer_" + layer_str);
        mask2 = gauss_7x7(mask2, "mask2_layer_" + layer_str);
    }

    // add the highest pyramid layer (lowest frequency band)

    accumulator(x, y) += i32(blurred1(x,y) * mask1(x, y)) + i32(blurred2(x,y) * mask2(x, y));

    output(x,y) = u16_sat(accumulator(x,y));

    ///////////////////////////////////////////////////////////////////////////
    // schedule
    ///////////////////////////////////////////////////////////////////////////

    init_mask1.compute_root().parallel(y).vectorize(x, 16);

    accumulator.compute_root().parallel(y).vectorize(x, 16);

    for (int layer = 0; layer < num_layers; layer++) {

        accumulator.update(layer).parallel(y).vectorize(x, 16);
    }

    return output;
}

Func tone_map(Func input, int width, int height, int gain) {

    Func grayscale("grayscale");
    Func brighter("brighter_grayscale");
    Func normal_dist("luma_weight_distribution");
    Func output("tone_map_output");
    
    Var x, y, c, v;
    RDom r(0, 3);

    // use grayscale and brighter grayscale images for exposure fusion

    grayscale(x, y) = u16(sum(u32(input(x, y, r))) / 3);

    brighter(x, y) = u16_sat(gain * u32(grayscale(x, y)));

    // gamma correct before combining

    Func gamma_grayscale = gamma_correct(grayscale);
    Func gamma_brighter = gamma_correct(brighter);

    // distribution function (from exposure fusion paper)
    
    normal_dist(v) = f32(exp(-12.5f * pow(f32(v) / 65535.f - .6f, 2.f)));

    // combine and invert gamma correction

    Func combine_output = combine(gamma_grayscale, gamma_brighter, width, height, normal_dist);
    Func linear_combine_output = gamma_inverse(combine_output);

    // reintroduce image color

    output(x, y, c) = u16_sat(u32(input(x, y, c)) * u32(linear_combine_output(x, y)) / max(1, grayscale(x, y)));

    ///////////////////////////////////////////////////////////////////////////
    // schedule
    ///////////////////////////////////////////////////////////////////////////
    
    grayscale.compute_root().parallel(y).vectorize(x, 16);
    
    normal_dist.compute_root().vectorize(v, 16);

    return output;
}

Func chroma_denoise(Func input, int width, int height) {
    Func output("chroma_denoise_output");
    Var x, y, c;
    Func yuv_input = rgb_to_yuv(input);
    Func denoised = median_filter_3x3(yuv_input);
    output = yuv_to_rgb(denoised);

    ///////////////////////////////////////////////////////////////////////////
    // schedule
    ///////////////////////////////////////////////////////////////////////////

    return output;
}

Func srgb(Func input) {
    
    Func srgb_matrix("srgb_matrix");
    Func output("srgb_output");

    Var x, y, c;
    RDom r(0, 3);               // reduction over color channels

    // srgb conversion matrix; values taken from dcraw sRGB profile conversion

    srgb_matrix(x, y) = 0.f;

    srgb_matrix(0, 0) =  1.964399f; srgb_matrix(1, 0) = -1.119710f; srgb_matrix(2, 0) =  0.155311f;
    srgb_matrix(0, 1) = -0.241156f; srgb_matrix(1, 1) =  1.673722f; srgb_matrix(2, 1) = -0.432566f;
    srgb_matrix(0, 2) =  0.013887f; srgb_matrix(1, 2) = -0.549820f; srgb_matrix(2, 2) =  1.535933f;

    // resulting (linear) srgb image

    output(x, y, c) = u16_sat(sum(srgb_matrix(r, c) * input(x, y, r)));

    ///////////////////////////////////////////////////////////////////////////
    // schedule
    ///////////////////////////////////////////////////////////////////////////

    srgb_matrix.compute_root().parallel(y).parallel(x);

    return output;
}

Func u8bit_interleaved(Func input) {

    Func output("8bit_interleaved_output");

    Var c, x, y;

    // Convert to 8 bit

    output(c, x, y) = u8_sat(input(x, y, c) / 256);

    ///////////////////////////////////////////////////////////////////////////
    // schedule
    ///////////////////////////////////////////////////////////////////////////

    output.compute_root().parallel(y).vectorize(x, 16);

    return output;
}

Func finish(Func input, int width, int height, const BlackPoint bp, const WhitePoint wp, const WhiteBalance &wb) {

    // 1. Black-layer subtraction and white-layer scaling
    Func black_white_point_output = black_white_point(Func(input), bp, wp);

    // 2. White balancing
    Func white_balance_output = white_balance(black_white_point_output, width, height, wb);

    // 3. Demosaicking
    Func demosaic_output = demosaic(white_balance_output, width, height);

    // 4. Chroma denoising
    //Func chroma_denoised_output = chroma_denoise(demosaic_output, width, height);

    // 5. sRGB color correction
    Func srgb_output = srgb(demosaic_output);
    
    // 6. Tone mapping
    Func tone_map_output = tone_map(srgb_output, width, height, 4);

    // 7. Gamma correction
    Func gamma_correct_output = gamma_correct(tone_map_output);

    // 8. Global contrast increase
    // TODO

    // 9. Sharpening
    // TODO

    // 10. Convert to 8 bit interleaved image
    return u8bit_interleaved(gamma_correct_output);
}