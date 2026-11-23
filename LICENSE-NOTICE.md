# Third-party notices

CINEMAX is built with:

* **JUCE 9** (https://juce.com) is dual-licensed under the JUCE licence or the **AGPLv3**.
  If you distribute CINEMAX binaries, you must either hold a JUCE licence that
  covers your use, or comply with the AGPLv3. The AGPLv3 route includes making the
  complete corresponding source code of CINEMAX available to recipients.
* **Microsoft Edge WebView2 SDK** (NuGet `Microsoft.Web.WebView2`) is provided under
  the Microsoft software licence terms included in the package.
* **React** (MIT), **Vite** (MIT), **vite-plugin-singlefile** (MIT) and **TypeScript**
  (Apache-2.0) are used for the user interface.
* **Intel Graphics Control Library header** (`third_party/igcl/igcl_api.h`, from
  https://github.com/intel/drivers.gpu.control-library) is used under the Intel Software
  License Agreement reproduced in `third_party/igcl/LICENSE.txt`: "Copyright (C) 2025 Intel
  Corporation". CINEMAX loads Intel's `ControlLib.dll` (installed with Intel graphics
  drivers) at run time and uses it only on Intel platforms. Nothing from Intel is bundled
  besides the header.
* **SADIE II HRTF database** (University of York, AudioLab) - the six real-ear sets in
  `hrtf\` are converted from it. D1, D2 and H12 are Apache-2.0; H3, H7 and H17 are under
  York's Non-Commercial Licence (free use and free redistribution with the licence text).
  Full texts and the citation are in `hrtf\LICENCES.txt`.
* **MediaPipe BlazeFace** (Google, Apache-2.0; ONNX conversion by Unity, Apache-2.0) -
  `models/blaze_face_short_range.onnx`, used by the optional webcam head tracking.
* **ONNX Runtime 1.32** (Microsoft, MIT) - `onnxruntime.dll`, loaded only for head
  tracking. Licence and third-party notices are in `models\`.
* **CINEMAX Mobile** ships the same three: the real-ear sets in `assets/hrtf`, the face model
  in `assets/models` (with ONNX Runtime's licence and notices) and ONNX Runtime for Android
  (`libonnxruntime.so`, from the same NuGet package), loaded only for front-camera head tracking.
* **VB-CABLE** (VB-Audio Software) is *not* bundled. Users install it themselves
  from https://vb-audio.com/Cable/ under VB-Audio's terms (donationware).

"IMAX" is a trademark of IMAX Corporation and "Dolby" is a trademark of Dolby
Laboratories. CINEMAX is not affiliated with, endorsed by or certified by either.
The "IMAX-STYLE" preset is only a descriptive name for a wide, large-screen-like
sound simulation. Likewise "DOLBY-STYLE" only describes CINEMAX's own processing tuned toward the
character reviewers describe for Dolby Atmos on headphones; it contains no Dolby
technology.
