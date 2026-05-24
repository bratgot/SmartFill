# Third-Party Licenses

This document lists every third-party component bundled with or
required by nuke-ai-fill, along with attribution and license terms
required by their respective licenses.

## Summary

| Component             | License        | Status in distribution    |
|-----------------------|----------------|---------------------------|
| stable-diffusion.cpp  | MIT            | Statically linked         |
| ggml / ggml-cuda      | MIT            | Statically linked         |
| libwebp               | BSD-3-Clause   | Statically linked         |
| miniz                 | Unlicense      | Statically linked         |
| stb_image*            | Public Domain  | Statically linked         |
| zip                   | Unlicense      | Statically linked         |
| ONNX Runtime          | MIT            | Distributed as DLL        |
| Foundry Nuke NDK      | Proprietary    | Linked dynamically; not redistributed |
| NVIDIA CUDA runtime   | NVIDIA EULA    | Linked dynamically; not redistributed by default |
| LaMa weights (Carve)  | Apache 2.0     | Model file; bundled with installer or downloaded by user |
| FLUX.1-schnell        | Apache 2.0     | Model file; downloaded by user |
| FLUX VAE              | Apache 2.0     | Model file; downloaded by user |
| CLIP-L text encoder   | MIT            | Model file; downloaded by user |
| T5-XXL encoder        | Apache 2.0     | Model file; downloaded by user |

All bundled components and recommended model files are permissive
open-source licenses (MIT, Apache 2.0, BSD-3-Clause, Unlicense,
Public Domain). nuke-ai-fill itself is MIT-licensed (see LICENSE).

Commercial use is permitted by every license above. See LICENSING.md
for the practical guidance.

---

## Code: components statically linked into the plugin DLLs

### stable-diffusion.cpp

- Upstream: https://github.com/leejet/stable-diffusion.cpp
- License: MIT
- Vendored at `third_party/stable-diffusion.cpp/` in the source tree
- Built as a static library and linked into AIGenerate.dll

```
MIT License

Copyright (c) 2023 leejet

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### ggml and ggml-cuda

- Upstream: https://github.com/ggerganov/ggml
- License: MIT
- Vendored as a submodule of stable-diffusion.cpp at
  `third_party/stable-diffusion.cpp/ggml/`
- Built as static libraries; ggml-cuda compiled with CUDA 12.x kernels
  for the target compute capability

```
MIT License

Copyright (c) 2023-2024 The ggml authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### libwebp

- Upstream: https://chromium.googlesource.com/webm/libwebp
- License: BSD-3-Clause
- Vendored inside stable-diffusion.cpp at
  `third_party/stable-diffusion.cpp/thirdparty/libwebp/`
- Used by sd.cpp for image format support

```
Copyright (c) 2010, Google Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

  * Neither the name of Google nor the names of its contributors may
    be used to endorse or promote products derived from this software
    without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### stb_image, stb_image_resize, stb_image_write

- Upstream: https://github.com/nothings/stb
- License: Public Domain (Unlicense) or MIT, choice of recipient
- Vendored inside stable-diffusion.cpp at
  `third_party/stable-diffusion.cpp/thirdparty/`
- Used by sd.cpp for image I/O

No notice required by either license. Author: Sean Barrett.

### miniz

- Upstream: https://github.com/richgel999/miniz
- License: MIT
- Vendored inside stable-diffusion.cpp at
  `third_party/stable-diffusion.cpp/thirdparty/miniz.h`

### zip (kuba--/zip)

- Upstream: https://github.com/kuba--/zip
- License: Unlicense (Public Domain dedication)
- Vendored inside stable-diffusion.cpp at
  `third_party/stable-diffusion.cpp/thirdparty/zip.c|.h`

No notice required.

---

## Code: components distributed as DLLs alongside the plugin

### ONNX Runtime

- Upstream: https://github.com/microsoft/onnxruntime
- License: MIT
- Version: 1.20.1 (CUDA EP build)
- Distributed as `onnxruntime.dll`,
  `onnxruntime_providers_cuda.dll`,
  `onnxruntime_providers_shared.dll`,
  `onnxruntime_providers_tensorrt.dll`
- Required by AISmartFill for LaMa inference

```
MIT License

Copyright (c) Microsoft Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Code: components linked dynamically, NOT redistributed

### The Foundry Nuke NDK

- License: Proprietary (Foundry Visionmongers Ltd)
- The plugin links against Nuke's NDK headers and libraries at build
  time. No NDK code is included in this repository or any
  distribution bundle.
- End users must have a valid Nuke license to use the plugin.

### NVIDIA CUDA runtime

- License: NVIDIA CUDA Toolkit EULA
- The plugin's ggml-cuda backend requires the CUDA runtime DLLs
  (`cudart64_12.dll`, `cublas64_12.dll`, `cublasLt64_12.dll`) at
  runtime.
- By default these are not redistributed; the user must have an
  appropriate NVIDIA driver installed (any 12.x driver provides
  the runtime).
- The optional `-IncludeCuda` flag to `package.ps1` will bundle the
  redistributable subset of these DLLs in the distribution zip.
  When using that option, end users implicitly accept the NVIDIA
  CUDA Toolkit EULA. See:
  https://docs.nvidia.com/cuda/eula/index.html

---

## Model weights: downloaded by the user

These files are not redistributed by nuke-ai-fill by default. The
plugin discovers them in the user's models directory at runtime.
If a distribution bundle includes them (via `package.ps1
-IncludeModels`), the attribution requirements below apply.

### LaMa weights (Carve ONNX export)

- Upstream: https://huggingface.co/Carve/LaMa-ONNX
- File: `lama_fp32.onnx`
- License: Apache 2.0
- Original model: https://github.com/advimman/lama (Suvorov et al.,
  "Resolution-robust Large Mask Inpainting with Fourier Convolutions",
  WACV 2022), also Apache 2.0
- This is an ONNX export of the original "big-lama" PyTorch weights

Required notice: see "Apache License 2.0" full text below.

### FLUX.1-schnell

- Upstream: https://huggingface.co/black-forest-labs/FLUX.1-schnell
- GGUF reformat: https://huggingface.co/leejet/FLUX.1-schnell-gguf
  (the GGUF this plugin loads)
- File: `flux1-schnell-q4_0.gguf` (or other Q-level from leejet's repo)
- License: Apache 2.0
- Copyright Black Forest Labs Inc.

WARNING: nuke-ai-fill defaults to FLUX.1-schnell because it is
Apache 2.0 licensed and permits commercial use. The author's other
"FLUX.1 [dev]" series of models look similar but are under the
"FLUX.1 [dev] Non-Commercial License v1.1.1" and require a separate
paid commercial license from Black Forest Labs for any business use.
Do not substitute a [dev] model file unless you have arranged that
license with BFL.

Required notice: see "Apache License 2.0" full text below.

### FLUX VAE (ae.safetensors)

- Upstream: https://huggingface.co/black-forest-labs/FLUX.1-schnell
- File: `ae.safetensors`
- License: Apache 2.0
- Copyright Black Forest Labs Inc.
- Released alongside FLUX.1-schnell under the same terms.

### CLIP-L text encoder (clip_l.safetensors)

- Upstream original: OpenAI CLIP, https://github.com/openai/CLIP
- File: `clip_l.safetensors`
- License: MIT
- Copyright (c) 2021 OpenAI

```
MIT License

Copyright (c) 2021 OpenAI

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### T5-XXL text encoder

- Upstream original: Google T5, https://github.com/google-research/text-to-text-transfer-transformer
- GGUF reformat: https://huggingface.co/city96/t5-v1_1-xxl-encoder-gguf
- File: `t5-v1_1-xxl-encoder-Q8_0.gguf`
- License: Apache 2.0
- Copyright Google Research

Required notice: see "Apache License 2.0" full text below.

---

## Apache License 2.0 (full text)

The full text below is required by Apache 2.0 for any component
listed above as Apache 2.0 licensed (LaMa weights, FLUX.1-schnell,
FLUX VAE, T5-XXL encoder).

```
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for describing the origin of the Work and
      reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS
```

