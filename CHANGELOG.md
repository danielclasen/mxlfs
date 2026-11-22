# Changelog

## [0.1.0](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.11...v0.1.0) (2026-11-22)


### Miscellaneous

* cut 0.1.0 as the first stable release ([#96](https://github.com/danielclasen/mxlfs/issues/96)) ([cf5ed7e](https://github.com/danielclasen/mxlfs/commit/cf5ed7e17a4fabc396f5f5ece3db670a8d7ed5d2))

## [0.1.0-rc.11](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.10...v0.1.0-rc.11) (2026-11-21)


### Bug Fixes

* **docs:** reboot Debian into the backports kernel before modprobe ([#97](https://github.com/danielclasen/mxlfs/issues/97)) ([2bbd1ef](https://github.com/danielclasen/mxlfs/commit/2bbd1ef0f5840008c6600269f7dc9900bbd75365))

## [0.1.0-rc.10](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.9...v0.1.0-rc.10) (2026-11-21)


### Bug Fixes

* **docs:** name the build tools and run dkms status as root ([#94](https://github.com/danielclasen/mxlfs/issues/94)) ([0bd2030](https://github.com/danielclasen/mxlfs/commit/0bd20309eaea983f20ea8786aa09a5012828a095))

## [0.1.0-rc.9](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.8...v0.1.0-rc.9) (2026-11-21)


### Bug Fixes

* **dev:** boot the FLAVOR's kernel tree in vm.sh ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **dev:** default ci-images.sh kernel's version as kernel-cache.sh does ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **dev:** fetch sparse again when its download fails ([7fb5eef](https://github.com/danielclasen/mxlfs/commit/7fb5eef04bd1c1a177aa1f636e217d12aad2ad00))
* **dev:** find bench-kernel.sh's directory before the cd to BUILD_DIR ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **dev:** keep host.sh's exit status when the cleanup fails ([#64](https://github.com/danielclasen/mxlfs/issues/64)) ([f7eaab0](https://github.com/danielclasen/mxlfs/commit/f7eaab0f7981dd7745b6be35f43b823968de6c21))
* **dev:** keep libmxl.sh from replacing the cached development image ([#63](https://github.com/danielclasen/mxlfs/issues/63)) ([558a6c0](https://github.com/danielclasen/mxlfs/commit/558a6c0cc434d41f9acefde9a5031d7977dcdb5d))
* **dev:** pass run.sh's build/ mount as one argument ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **dev:** print the usage of ci-images.sh and kernel-cache.sh ([7fb5eef](https://github.com/danielclasen/mxlfs/commit/7fb5eef04bd1c1a177aa1f636e217d12aad2ad00))
* **dev:** refuse a FLAVOR other than debug or perf ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **dev:** remove kernel-src.sh's partial download on INT and TERM ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **dev:** resolve module.sh's kernel path before changing directory ([7fb5eef](https://github.com/danielclasen/mxlfs/commit/7fb5eef04bd1c1a177aa1f636e217d12aad2ad00))
* **dev:** write bench-kernel.sh's module list into its build tree ([#65](https://github.com/danielclasen/mxlfs/issues/65)) ([d45a2ec](https://github.com/danielclasen/mxlfs/commit/d45a2ec41157df2fd15905db806a6754c8915125))
* **kernel:** allocate a wanted owner record only when it is missing ([8cb4549](https://github.com/danielclasen/mxlfs/commit/8cb4549d1405586ca20d3cdd84289642b9d3404f))
* **kernel:** allocate the wanted tables only for mounts with wait= ([8cb4549](https://github.com/danielclasen/mxlfs/commit/8cb4549d1405586ca20d3cdd84289642b9d3404f))
* **kernel:** check a view's size again under its lock ([8cb4549](https://github.com/danielclasen/mxlfs/commit/8cb4549d1405586ca20d3cdd84289642b9d3404f))
* **kernel:** decide whether a file is a view where it is made one ([8cb4549](https://github.com/danielclasen/mxlfs/commit/8cb4549d1405586ca20d3cdd84289642b9d3404f))
* **kernel:** end fallocate on a fatal signal ([4724e21](https://github.com/danielclasen/mxlfs/commit/4724e21fa4d2eeb93dbe15c3c19db83bbe28fdd8))
* **kernel:** join only a pending wanted entry ([8cb4549](https://github.com/danielclasen/mxlfs/commit/8cb4549d1405586ca20d3cdd84289642b9d3404f))
* **kernel:** keep a page with a memory error out of MOVE and the fold ([#57](https://github.com/danielclasen/mxlfs/issues/57)) ([85351ce](https://github.com/danielclasen/mxlfs/commit/85351cea4e6f8b087eb2e787b3d4b569cf6d5c01))
* **kernel:** keep MXLFS_IOC_WANTED_NEXT reports and paths intact ([#46](https://github.com/danielclasen/mxlfs/issues/46)) ([5cfba52](https://github.com/danielclasen/mxlfs/commit/5cfba529bbed3a7a2b7bfc6e53eda67538a5419e))
* **kernel:** let RESOLVE_NO_XDEV walks fail missing flows with ENOENT ([#39](https://github.com/danielclasen/mxlfs/issues/39)) ([46a7763](https://github.com/danielclasen/mxlfs/commit/46a77633d388a5a9ef698e887813cd4398bfc092))
* **kernel:** refuse a fold while a folio has references beyond its own ([#40](https://github.com/danielclasen/mxlfs/issues/40)) ([f564eab](https://github.com/danielclasen/mxlfs/commit/f564eabaf69f9bc7eda6a10a0fbb4c4be1a07f59))
* **kernel:** refuse a poisoned page through a view ([#55](https://github.com/danielclasen/mxlfs/issues/55)) ([d88e8eb](https://github.com/danielclasen/mxlfs/commit/d88e8eb80f7c91dd33a9d7adebd039ed61924d51))
* **kernel:** refuse a remount that changes the mount's options ([8cb4549](https://github.com/danielclasen/mxlfs/commit/8cb4549d1405586ca20d3cdd84289642b9d3404f))
* **kernel:** refuse ioctl input the module does not use ([#38](https://github.com/danielclasen/mxlfs/issues/38)) ([8d20bf0](https://github.com/danielclasen/mxlfs/commit/8d20bf0215d26d700ac3cefa9d9b8a53731acc1d))
* **kernel:** refuse read, write and splice of a page with a memory error ([#52](https://github.com/danielclasen/mxlfs/issues/52)) ([54d9ba7](https://github.com/danielclasen/mxlfs/commit/54d9ba72d59974f9c7dfceec62eaec658ab25f5b))
* **kernel:** run fallocate's checks for MXLFS_IOC_ALLOCATE ([#42](https://github.com/danielclasen/mxlfs/issues/42)) ([466bbfa](https://github.com/danielclasen/mxlfs/commit/466bbfacac1e2cc50e4d0080c8a104efa4fc4420))
* **kernel:** splice from a view through its read_iter ([8cb4549](https://github.com/danielclasen/mxlfs/commit/8cb4549d1405586ca20d3cdd84289642b9d3404f))
* **kernel:** test only a folio whose order and index match its entry ([#58](https://github.com/danielclasen/mxlfs/issues/58)) ([e1df94c](https://github.com/danielclasen/mxlfs/commit/e1df94cd90ee0d67a2e26cb183411a50238e620c))
* **kernel:** treat a folio that is not uptodate as a hole ([#37](https://github.com/danielclasen/mxlfs/issues/37)) ([eecf2ba](https://github.com/danielclasen/mxlfs/commit/eecf2bae6701c43764b8803f8253e040dd55e0a5))
* **kernel:** unhash a flow's negative dentry before waking its waiters ([8cb4549](https://github.com/danielclasen/mxlfs/commit/8cb4549d1405586ca20d3cdd84289642b9d3404f))
* **kernel:** wait for a view's folio lock without the mmap lock ([#53](https://github.com/danielclasen/mxlfs/issues/53)) ([4683ba7](https://github.com/danielclasen/mxlfs/commit/4683ba7e7e09390c103073250f991b28bf5fc5b0))
* **kernel:** write WANTED_NEXT's timeout_ms back as passed ([4724e21](https://github.com/danielclasen/mxlfs/commit/4724e21fa4d2eeb93dbe15c3c19db83bbe28fdd8))
* **kernel:** zero a larger MXLFS_IOC_STATS struct and refuse short ones ([#43](https://github.com/danielclasen/mxlfs/issues/43)) ([fa659ac](https://github.com/danielclasen/mxlfs/commit/fa659ac3f371ef70f4d85372b5845d88b27c417c))
* **kmm:** let the in-cluster build run under a stacked AppArmor label ([#36](https://github.com/danielclasen/mxlfs/issues/36)) ([5cd4215](https://github.com/danielclasen/mxlfs/commit/5cd42154c2ce202c0f82f45094c3e331fa5a5aa8))
* **tools:** exit when a harness reader cannot read the flow's head ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** fail on a missing meminfo key and a refused LAZY_ALLOC off ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** fold in foldmem only once its writer has stored ([#92](https://github.com/danielclasen/mxlfs/issues/92)) ([34831ca](https://github.com/danielclasen/mxlfs/commit/34831ca84c98f5c47c7240225f82f95357580806))
* **tools:** give the audio merger's silence source its slot in upto ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** keep setcheck's latency signed and its sample bounded ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))
* **tools:** log the first five faults of each setcheck counter ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** name the operation in each error setcheck returns ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** pause before setcheck reopens a flow replaced while joining ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** print none for an empty latency set in the view bench ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** refuse a harness wanted argument other than fail ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))
* **tools:** refuse flags of the other setcheck command ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))
* **tools:** refuse setcheck formats and members it cannot write ([#66](https://github.com/danielclasen/mxlfs/issues/66)) ([c2387bc](https://github.com/danielclasen/mxlfs/commit/c2387bc234f3bf16a9df52fb89b9e9f37222e352))
* **tools:** report harness failures as what they are ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))
* **tools:** report harness failures that set no errno without strerror ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** report the error posix_fallocate returns ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))
* **tools:** start both mergers' view flow with one helper ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))
* **tools:** stop setcheck's late-index loop on a failed Runtime() ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))


### Code Refactoring

* **kernel:** adopt mmap_prepare and a 2 MiB fault window ([#54](https://github.com/danielclasen/mxlfs/issues/54)) ([2120eb8](https://github.com/danielclasen/mxlfs/commit/2120eb8bd9e551b7ed39d63cb369bce398a6e407))
* **kernel:** assert the inode lock in mxlfs_move_block ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **kernel:** assert the locks helpers say they are called under ([4724e21](https://github.com/danielclasen/mxlfs/commit/4724e21fa4d2eeb93dbe15c3c19db83bbe28fdd8))
* **kernel:** clamp a view read to its page once ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **kernel:** fold folio uptodate, dirty and count into one helper ([#62](https://github.com/danielclasen/mxlfs/issues/62)) ([7b52aed](https://github.com/danielclasen/mxlfs/commit/7b52aed37bbb88a951277e431e349cd5e7b8679f))
* **kernel:** give mxlfs_get_inode the device number, as ramfs does ([4724e21](https://github.com/danielclasen/mxlfs/commit/4724e21fa4d2eeb93dbe15c3c19db83bbe28fdd8))
* **kernel:** keep a view's read path and selection in view.c ([4724e21](https://github.com/danielclasen/mxlfs/commit/4724e21fa4d2eeb93dbe15c3c19db83bbe28fdd8))
* **kernel:** mark the stores of an inode's lazy flag ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **kernel:** name the lazy predicate after what it asks ([4724e21](https://github.com/danielclasen/mxlfs/commit/4724e21fa4d2eeb93dbe15c3c19db83bbe28fdd8))
* **kernel:** split mxlfs.c into one file per subsystem ([#61](https://github.com/danielclasen/mxlfs/issues/61)) ([2f0802a](https://github.com/danielclasen/mxlfs/commit/2f0802a860d80ece23151d06f23b75f7431fe90c))
* **kernel:** test a whole folio for memory errors in one helper ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** create the harness's flow writers through one helper ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** create the harness's test files through one helper ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** drop the cmd_ prefix from the audio merger's back ends ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** end both readers' summaries through one helper ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** end setcheck's read through one channel and one error ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))
* **tools:** give the harness's shared settings one default each ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))
* **tools:** log the writer's format from its flow in setcheck ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))
* **tools:** print the window histogram by its own length ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))
* **tools:** size lazy files and read stats through shared helpers ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))
* **tools:** split the harness into one file per command group ([#59](https://github.com/danielclasen/mxlfs/issues/59)) ([2a4087f](https://github.com/danielclasen/mxlfs/commit/2a4087f84657007e33c2ed77ceaab9e7f14f76fd))
* **tools:** stop viewrace's local flag shadowing the harness's stop ([d0377fb](https://github.com/danielclasen/mxlfs/commit/d0377fb3d0e2a29a360118d904721cfb067fa398))


### Build System

* **dev:** enable memory failure handling in the test kernels ([#49](https://github.com/danielclasen/mxlfs/issues/49)) ([504f4e5](https://github.com/danielclasen/mxlfs/commit/504f4e525b393183bef4c15dae5f6d0384421f60))
* **tools:** define _GNU_SOURCE for every harness file ([c9b802b](https://github.com/danielclasen/mxlfs/commit/c9b802be9db30adc0648767a88b3b589b2bc6431))


### Continuous Integration

* check out with actions/checkout v7 in every workflow ([7fb5eef](https://github.com/danielclasen/mxlfs/commit/7fb5eef04bd1c1a177aa1f636e217d12aad2ad00))
* **checkpatch:** fetch the pinned checkpatch through the GitHub API ([#33](https://github.com/danielclasen/mxlfs/issues/33)) ([b1331b9](https://github.com/danielclasen/mxlfs/commit/b1331b9e1096eb188a0421c76e03c005d9fb5f6e))
* fail when setcheck's go.mod and Dockerfiles name other go-mxl ([7fb5eef](https://github.com/danielclasen/mxlfs/commit/7fb5eef04bd1c1a177aa1f636e217d12aad2ad00))
* report QEMU's exit status and run states on a VM failure ([#91](https://github.com/danielclasen/mxlfs/issues/91)) ([205816c](https://github.com/danielclasen/mxlfs/commit/205816cebeff136f6e72b50638eeb7cc94ecee66))
* report the stage, status and guest console of a failed VM test ([#60](https://github.com/danielclasen/mxlfs/issues/60)) ([5ca21f8](https://github.com/danielclasen/mxlfs/commit/5ca21f89ff51285f2023c1a0264581980364a677))


### Miscellaneous

* **dev:** set transparent huge pages to madvise with one write ([#68](https://github.com/danielclasen/mxlfs/issues/68)) ([b0e1d11](https://github.com/danielclasen/mxlfs/commit/b0e1d112322ae9edff4f89719afcb511f6602732))

## [0.1.0-rc.8](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.7...v0.1.0-rc.8) (2026-11-20)


### Features

* **dkms:** ship the module as a DKMS package for Debian and Ubuntu ([#24](https://github.com/danielclasen/mxlfs/issues/24)) ([d6dceeb](https://github.com/danielclasen/mxlfs/commit/d6dceebc293c750fa1fba9c55d092bdf200c6083))
* **kernel:** build on Linux 6.19 and newer ([b771981](https://github.com/danielclasen/mxlfs/commit/b77198173e33b1c34491b219366dec12ccbaa870))
* **kmm:** build and load the module through KMM on Ubuntu and Debian ([#25](https://github.com/danielclasen/mxlfs/issues/25)) ([8f1bf66](https://github.com/danielclasen/mxlfs/commit/8f1bf66300bba66d11aec561c040e21b890500fb))


### Bug Fixes

* **kernel:** bound and charge a view's per-page selection ([27c6472](https://github.com/danielclasen/mxlfs/commit/27c64724a07a06492e8d5fcd32a66c4686043bb2))
* **kernel:** bound the missing flows path walks wait for ([27c6472](https://github.com/danielclasen/mxlfs/commit/27c64724a07a06492e8d5fcd32a66c4686043bb2))
* **kernel:** charge per-file and per-process memory to the caller ([27c6472](https://github.com/danielclasen/mxlfs/commit/27c64724a07a06492e8d5fcd32a66c4686043bb2))
* **kernel:** clear a move's results with memset_after() ([b771981](https://github.com/danielclasen/mxlfs/commit/b77198173e33b1c34491b219366dec12ccbaa870))


### Continuous Integration

* **release:** let a failed release job re-run and narrow deb's token ([#30](https://github.com/danielclasen/mxlfs/issues/30)) ([67ddea4](https://github.com/danielclasen/mxlfs/commit/67ddea485cafbe60c237405a04b16a51dba7625c))
* share kernel trees and the dev image through the registry ([#27](https://github.com/danielclasen/mxlfs/issues/27)) ([24491a8](https://github.com/danielclasen/mxlfs/commit/24491a824ede27a9757572c179ba03112bfdb486))

## [0.1.0-rc.7](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.6...v0.1.0-rc.7) (2026-11-08)


### Performance

* **kernel:** copy a folio before its move window ([#20](https://github.com/danielclasen/mxlfs/issues/20)) ([97ee5a8](https://github.com/danielclasen/mxlfs/commit/97ee5a89bdb40e81c1ba59ee3a335b4667800391))

## [0.1.0-rc.6](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.5...v0.1.0-rc.6) (2026-11-07)


### Features

* **kernel:** prefer, move and hold a file's pages per NUMA node ([#17](https://github.com/danielclasen/mxlfs/issues/17)) ([bd84e3f](https://github.com/danielclasen/mxlfs/commit/bd84e3fa846b95317eb28daee2987db3a2daea2e))


### Bug Fixes

* **kernel:** keep a folded range's small folios locked until dropped ([#16](https://github.com/danielclasen/mxlfs/issues/16)) ([ab2686b](https://github.com/danielclasen/mxlfs/commit/ab2686b3ad3b1b23167797d4fc3e558a5ab316cf))

## [0.1.0-rc.5](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.4...v0.1.0-rc.5) (2026-11-07)


### Features

* **kernel:** prefer one NUMA node for every page of a mount ([#14](https://github.com/danielclasen/mxlfs/issues/14)) ([b2b411d](https://github.com/danielclasen/mxlfs/commit/b2b411d119ae1bfc0091edb030cbd7c38b034729))

## [0.1.0-rc.4](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.3...v0.1.0-rc.4) (2026-11-05)


### Features

* **kernel:** give a file's tail a PMD folio past EOF with huge=always ([#11](https://github.com/danielclasen/mxlfs/issues/11)) ([032e7da](https://github.com/danielclasen/mxlfs/commit/032e7dac3b98129b0279192f8142fbbe210a0d62))


### Bug Fixes

* **dev:** give the dev container the group that owns /dev/kvm ([#10](https://github.com/danielclasen/mxlfs/issues/10)) ([5a1143c](https://github.com/danielclasen/mxlfs/commit/5a1143c436b83e80c5e3d43efbdfbf125b872ac2))
* **dev:** mount a symlinked build/ in libmxl.sh ([#10](https://github.com/danielclasen/mxlfs/issues/10)) ([5a1143c](https://github.com/danielclasen/mxlfs/commit/5a1143c436b83e80c5e3d43efbdfbf125b872ac2))


### Build System

* **dev:** build libmxl from dmf/main-next b3c5fd4 ([#10](https://github.com/danielclasen/mxlfs/issues/10)) ([5a1143c](https://github.com/danielclasen/mxlfs/commit/5a1143c436b83e80c5e3d43efbdfbf125b872ac2))

## [0.1.0-rc.3](https://github.com/danielclasen/mxlfs/compare/v0.1.0-rc.2...v0.1.0-rc.3) (2026-10-26)


### Features

* **kernel:** replace one source of a view in place ([#7](https://github.com/danielclasen/mxlfs/issues/7)) ([c606188](https://github.com/danielclasen/mxlfs/commit/c606188d796d25e01d12380b5228aa8282273372))


### Bug Fixes

* **harness:** retire view slots a lagging view would serve stale ([#5](https://github.com/danielclasen/mxlfs/issues/5)) ([41a124f](https://github.com/danielclasen/mxlfs/commit/41a124f00e9091bf30bdc243ca4031ffd9b9e529))

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
