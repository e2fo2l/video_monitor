-- video_monitor — C++20 reconstruction of the Dreame r2228 camera service
-- (original build: "commit-r2212_0041release-0-g8da91c04", OpenWrt/Linaro GCC 6.4, aarch64).
--
-- Robot build (see README.md for preparing the toolchain and sysroot):
--     xmake f -p linux -a arm64 -m release --sdk=$TOOLCHAIN --cross=aarch64-linux- \
--             --vendor_root=$SYSROOT -o build-robot && xmake
--
-- --compat=y builds the original behavior, bugs included.
--
-- PC build (vendor SDKs replaced by stubs/, for development and tools/host_test.sh):
--     xmake f -p linux -a x86_64 -m debug -o build && xmake

set_project("video_monitor")
set_version("1.0.0")
set_languages("c11", "cxx20")
add_rules("mode.debug", "mode.release")
set_warnings("all")

option("vendor_root")
    set_default("")
    set_showmenu(true)
    set_description("Robot rootfs containing libagora-rtc-sdk, libtrecorder, libnnxx, libnanomsg, ...")
option_end()

option("compat")
    set_default(false)
    set_showmenu(true)
    set_description("Behave exactly like the original binary, bugs included (ps-based self-detection, five hard-coded SPS/PPS sizes, IPC lock starvation)")
    add_defines("VM_COMPAT=1")
option_end()

option("breakpad")
    set_default(false)
    set_showmenu(true)
    set_description("Install the google-breakpad crash handler like the original binary does")
    add_defines("VM_WITH_BREAKPAD=1")
option_end()

add_requires("rapidjson")
if has_config("breakpad") then
    add_requires("breakpad")
end

local vendor_root = get_config("vendor_root")
local use_vendor = vendor_root ~= nil and vendor_root ~= ""

-- PC build: third-party libraries come from xmake-repo and are built by xmake (never taken from
-- the system), pinned close to the robot's versions (OpenSSL 1.1.1, curl 7.8x; curl 8.18+ no
-- longer builds against OpenSSL 1.1.1).
-- The robot build links the robot's own copies instead (see vendor_root).
local host_pkgs = {"openssl", "libcurl", "alsa-lib", "ffmpeg"}
if not use_vendor then
    add_requires("openssl 1.1.1-w", {system = false})
    add_requires("libcurl 7.87.0", {system = false, configs = {openssl = true}}) -- robot: 7.88.1
    add_requires("alsa-lib", {system = false})
    -- The robot has FFmpeg 4.x, but xmake-repo's 4.4.4 no longer builds with current binutils
    -- (x86 inline asm, fixed upstream in 4.4.5/6.1). The code handles both API generations.
    add_requires("ffmpeg 6.1", {system = false, configs = {gpl = false, ffmpeg = false, libdrm = false}})
end

-- Stand-ins for the proprietary vendor SDKs so the program links and runs on a PC.
target("vendor_stubs")
    set_kind("static")
    set_default(not use_vendor)
    add_includedirs("include", {public = true})
    add_files("stubs/*.cpp")
target_end()

target("video_monitor")
    set_kind("binary")
    add_includedirs("include", "src")
    add_files("src/*.cpp", "src/util/*.cpp", "src/util/*.c")
    add_options("compat", "breakpad")
    add_packages("rapidjson")
    if has_config("breakpad") then
        add_packages("breakpad")
    end
    add_syslinks("pthread", "dl", "rt")
    -- Export the default-visibility symbols (startMonitor/endMonitor, see monitor_video.h):
    -- LD_PRELOAD shims such as vacuumstreamer look them up in the main binary with dlsym().
    add_ldflags("-rdynamic", {force = true})

    if use_vendor then
        -- <vendor_root>/usr/lib: the robot's /usr/lib copied verbatim.
        -- <vendor_root>/usr/include: headers matching it (FFmpeg 4.x, OpenSSL 1.1, curl, ALSA),
        -- e.g. from Debian bullseye arm64 -dev packages, which use a multiarch include dir.
        local libdir = path.join(vendor_root, "usr/lib")
        add_sysincludedirs(path.join(vendor_root, "usr/include"),
                           path.join(vendor_root, "usr/include/aarch64-linux-gnu"))
        add_linkdirs(libdir)
        -- Vendor libs pull in more vendor libs (libawrecorder, libcdx_*, libVE, ...).
        add_ldflags("-Wl,-rpath-link," .. libdir, {force = true})
        -- libtrecorder does not declare its own deps (e.g. CdxMuxerCreate from libcdx_muxer),
        -- so the whole Allwinner media stack is linked explicitly, like the original binary.
        add_links("nnxx", "nanomsg", "asound", "agora-rtc-sdk", "MemAdapter", "uapi",
                  "avformat", "avcodec", "avutil", "curl", "ssl", "crypto",
                  "trecorder", "awrecorder", "vencoder", "aencoder", "cdx_base", "cdx_muxer",
                  "cdc_base", "VE", "vdecoder", "z")
    else
        add_deps("vendor_stubs")
        add_packages(table.unpack(host_pkgs))
    end
target_end()
