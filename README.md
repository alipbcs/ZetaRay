# ZetaRay
[![MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)

A hobby real-time renderer in Direct3D 12, mainly developed for learning and experimenting with the latest research in real-time rendering—with an emphasis on ray tracing. 

This renderer follows a hybrid approach where the primary surface is rasterized, but all the lighting is raytraced by leveraging GPU-accelerated hardware ray tracing.

<div align="center">
  <img src="Assets/Images/2.png?raw=true" alt="Sponza" style="zoom:60%"/>
  <p style="text-align: center;"><i>Sponza Atrium rendered by ZetaRay. Model downloaded from <a href="https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html">Intel Samples Library</a>.</i></p>
</div>

## Features

 - GPU-driven—bindless, indirect drawing with GPU-based occlusion culling
 - One bounce of indirect illumination using ReSTIR GI [[1](#references)]. Custom spatio-temporal denoisers for each BRDF layer (diffuse & specular) clean up the results.
 - ReSTIR DI [[5](#references)] for direct lighting from sky
 - Ray-traced sun soft shadows. A slightly modified FidelityFX denoiser [[2](#references)] is used for denoising.
 - Ray cones for texture MIP selection
 - Single scattering sky and atmosphere (based on [[3, 4](#references)])
 - Physically-based materials with roughness, metallic mask, normal and emissive maps
 - Render graph for automatic resource barrier placement and multi-threaded GPU command list recording and submission ([more details below](#render-graph))
 - Temporal anti-aliasing (TAA)
 - AMD FSR2 upscaling
 - Auto exposure using luminance histogram
 - glTF scene loading with the roughness-metalness workflow

### Render Graph

Modern APIs such as Vulkan and Direct3D 12 require barriers to be placed manually by the programmer to ensure correct resource synchronization. As program complexity grows, manual placement can lead to issues such as:
   - Becoming highly error-prone and limiting extensibility and prototyping.
   - GPU command buffers can be recorded in multiple threads; correct usage and prevention of data-race issues may be hard to achieve.
    
Furthermore, using multiple command buffers requires a correct submission order—e.g. command list A writes to a resource that is read by command list B, so A has to be submitted before B. 

A render graph can help with all of the above; by analyzing resource dependencies, a directed acyclic graph (DAG) is formed from which submission order is derived and barriers are automatically placed. Async compute is also supported. Note that inserted barriers only ensure correct usage and may not be optimal. A visualization is shown below.

<div align="center">
  <img src="Assets/Images/6.png?raw=true" alt="Render graph" style="zoom:50%"/>
  <p style="text-align: center;"><i>A sample frame render graph.</i></p>
</div>

## Requirements

1. A GPU with hardware-accelerated ray tracing support (NVIDIA RTX 2000 series or later, AMD RX 6000 series or later). Tested on RTX 3070 desktop and RTX 3060 laptop.
2. Windows 10 1909 or later (required by [Agility SDK](https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/)).
3. [Visual Studio 2019 or later](https://visualstudio.microsoft.com/downloads/).
4. [CMake 3.21](https://cmake.org/download/).

## Build

Standard CMake build. Make a build directory somewhere you like, then call CMake from inside that directory and point it to ([`./CMakeLists`](./CMakeLists.txt)) in the top-level project directory (`./build` is preferred as it's included in [`.gitignore`](./.gitignore)).

## Sample App

The main sample application ([`Samples/ZetaLab/`](./Samples/ZetaLab/)) works by loading a glTF scene and then proceeding to rendering that scene while exposing various renderer parameters and settings through the UI window. 

Note that glTF scenes need to be preprocessed first by generating mipmaps and converting textures to DDS format. A command-line utility app ([`Tools/BCnCompressglTF`](./Tools/BCnCompressglTF/)) is provided for that purpose. It can be used as follows:

```bash
> cd bin
> .\BCnCompressglTF <path-to-gltf>
```
The outputs are:
1. The converted glTF scene file in the same directory with a `_zeta` suffix (e.g. `myscene.gltf` -> `myscene_zeta.gltf`) 
2. The compressed textures in the `<path-to-gltf-directory>/compressed` directory.

For convenience, a preprocessed Cornell Box scene is provided ([`Assets/CornellBox/cornell9.gltf`](./Assets/CornellBox/cornell9.gltf)). After building the project, run it as follows:
```bash
> cd bin
> .\ZetaLab ..\Assets\CornellBox\cornell9.gltf
```

Currently, unicode paths are not supported. Support is planned for a future release.

## Screenshots

<div align="center">
  <img src="Assets/Images/4.jpg?raw=true" alt="San Miguel" style="zoom:50%"/>
  <p style="text-align: center;"><i>San Miguel 2.0. Downloaded from <a href="https://casual-effects.com/data">Morgan McGuire's Computer Graphics Archive</a>.</i></p>

  </br>

  <img src="Assets/Images/3.png?raw=true" alt="San Miguel" style="zoom:50%"/>
  <p style="text-align: center;"><i>San Miguel 2.0. Downloaded from <a href="https://casual-effects.com/data">Morgan McGuire's Computer Graphics Archive</a>.</i></p>

  </br>

  <img src="Assets/Images/7.png?raw=true" alt="San Miguel" style="zoom:50%"/>
  <p style="text-align: center;"><i>San Miguel 2.0. Downloaded from <a href="https://casual-effects.com/data">Morgan McGuire's Computer Graphics Archive</a>.</i></p>

  </br>

  <img src="Assets/Images/8.png?raw=true" alt="Sponza" style="zoom:50%"/>
  <p style="text-align: center;"><i>Sponza Atrium. Model downloaded from <a href="https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html">Intel Samples Library</a>.</i></p>

  </br>

  <img src="Assets/Images/5.png?raw=true" alt="Cornell Box" style="zoom:50%"/>
  <p style="text-align: center;"><i>Cornell Box. <a href="https://sketchfab.com/3d-models/cornell-box-original-0d18de8d108c4c9cab1a4405698cc6b6">Original scene</a> by <a href="https://sketchfab.com/t-ly">t-ly</a>.</i></p>
</div>

## References
[**1**] Y. Ouyang, S. Liu, M. Kettunen, M. Pharr and J. Pantaleoni, "ReSTIR GI: Path Resampling for Real-Time Path Tracing," *Computer Graphics Forum*, 2021.

[**2**] FidelityFX Denoiser, [https://github.com/GPUOpen-Effects/FidelityFX-Denoiser](https://github.com/GPUOpen-Effects/FidelityFX-Denoiser)

[**3**] S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique," *Computer Graphics Forum*, 2020.

[**4**] MinimalAtmosphere, [https://github.com/Fewes/MinimalAtmosphere](https://github.com/Fewes/MinimalAtmosphere)

[**5**] B. Bitterli, C. Wyman, M. Pharr, P. Shirley, A. Lefohn and W. Jarosz, "Spatiotemporal reservoir resampling for real-time ray tracing with 
dynamic direct lighting," *ACM Transactions on Graphics*, 2020.

## External Libraries

- [AMD FSR 2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2)
- [cgltf](https://github.com/jkuhlmann/cgltf)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [doctest](https://github.com/doctest/doctest)
- [FastDelegate](https://www.codeproject.com/Articles/7150/Member-Function-Pointers-and-the-Fastest-Possible)
- [ImPlot](https://github.com/epezent/implot)
- [imnodes](https://github.com/Nelarius/imnodes)
- [json](https://github.com/nlohmann/json)
- [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue)
- [stb](https://github.com/nothings/stb)
- [xxHash](https://github.com/Cyan4973/xxHash)

## License

This project is licensed under the MIT License.