#pragma once

/*!
 * What the temporal upscalers (FSR 3.1, DLSS, FSR 4) take from the frame, filled once per eye by
 * UpscalerStage::run. Each reads the fields it needs: DLSS the first six, FSR 3.1 adds the
 * depth and projection terms, FSR 4 the depth terms, tan_half_y and the sharpness.
 */
struct UpscaleInputs {
  unsigned colour = 0;    ///< render-size, single-sample scene colour texture
  unsigned depth = 0;     ///< render-size depth-stencil texture
  unsigned velocity = 0;  ///< RG16F velocity (MotionVectors), 0: none (zero motion, reset)
  bool reset = false;     ///< history is invalid (camera cut, load, resize)
  bool hdr = false;       ///< FP16 scene (values above 1)
  float frame_ms = 16.7f;
  float w_k0 = 0.f, w_k1 = 0.f;  ///< clip w = k0 / (ndc z - k1) (MotionVectors)
  float tan_half_x = 1.f, tan_half_y = 1.f;
  float sharpness = 0.8f;  ///< FSR 3.1's RCAS sharpness, shared
};
