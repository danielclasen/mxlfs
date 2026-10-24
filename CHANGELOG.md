# Changelog

## [0.1.0-rc.2](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.1...v0.1.0-rc.2) (2026-10-24)


### Bug Fixes

* **kernel:** name a waiting process in the reporter's pid namespace ([#3](https://github.com/danielclasen/mxlfs/issues/3)) ([068d46d](https://github.com/danielclasen/mxlfs/commit/068d46d758d624a0698644a3d64a62b342dbc053))

## 0.1.0-rc.1 (2026-10-24)


### Features

* add mxlfs module with source-switching views and a harness ([82d5709](https://github.com/danielclasen/mxlfs/commit/82d57096ff0bc47248bba8affb6500a4a18ef3dd))
* **kernel:** allocate a file on request, whatever the caller's mode ([4090095](https://github.com/danielclasen/mxlfs/commit/40900954e03a4d5c054eabe10e0389b89a421733))
* **kernel:** allocate lazily for placeholder flows ([ef5e73a](https://github.com/danielclasen/mxlfs/commit/ef5e73a17cf9efb9b55c1bcf2755cb049ab866d5))
* **kernel:** let path walks wait for a missing flow ([bdcef69](https://github.com/danielclasen/mxlfs/commit/bdcef692eb9ec9e267504dae05eabf744dea2d84))
* **kernel:** select a view's source per page range ([15e5b90](https://github.com/danielclasen/mxlfs/commit/15e5b90e6e27ac3549eb09dc4d2df05e0912bc9d))
* **tools:** add setcheck for redundancy tests on a cluster ([10cca30](https://github.com/danielclasen/mxlfs/commit/10cca30f7eb419833c0979e26bdacd6304d711f1))
* **tools:** serve a view from a sticky primary member ([8f3d05a](https://github.com/danielclasen/mxlfs/commit/8f3d05a2671699e79a746918c482f6eb69934451))


### Bug Fixes

* **kernel:** default the size limit to half of RAM, as tmpfs does ([2f6d15d](https://github.com/danielclasen/mxlfs/commit/2f6d15ddddbddd186dd27517fd511ccf0e08986d))
* **kernel:** let any process allocate lazily ([c1b32fa](https://github.com/danielclasen/mxlfs/commit/c1b32faad8758e49d7ab086502176db131354613))
* **kernel:** stop views from pinning the mount and fix their read lock order ([b1ddefb](https://github.com/danielclasen/mxlfs/commit/b1ddefb22576c713ef8f514238c5a861851699f1))
* **tools:** start setcheck reads at the flow's head ([7948b5d](https://github.com/danielclasen/mxlfs/commit/7948b5dcae0be42ad56a5eb44b99a6f7a3462e1b))
* **tools:** start the harness merge at the leading member's grain ([e1178c2](https://github.com/danielclasen/mxlfs/commit/e1178c2d4c52c99003fc3ac347c2d3f4f78c5664))
* **tools:** track setcheck's go.mod ([3a081ae](https://github.com/danielclasen/mxlfs/commit/3a081ae8f7a9858db7492ffa2233d83b889da0f8))


### Build System

* add a script that builds libmxl's test suite ([3cb8b56](https://github.com/danielclasen/mxlfs/commit/3cb8b5607efb2b19aca36a4b0f608841eb8ece78))
* cut rc releases with release-please ([#1](https://github.com/danielclasen/mxlfs/issues/1)) ([cd66331](https://github.com/danielclasen/mxlfs/commit/cd66331a55318015cffc0fa1a53362420b01243f))
* **dev:** build the bench kernel from the full distribution config ([02a6d55](https://github.com/danielclasen/mxlfs/commit/02a6d55c8882d2ba0704faa80406be261f49b30b))
* **dev:** install LLVM's binutils in the dev image ([56f56cf](https://github.com/danielclasen/mxlfs/commit/56f56cf140d951184f4aec4c1ffea9e474b23675))


### Miscellaneous

* license the module GPL-2.0-only and its uapi header with the syscall note ([59bc290](https://github.com/danielclasen/mxlfs/commit/59bc290a13bd93c23e122651a85a7abb214cb895))
