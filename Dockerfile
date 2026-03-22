FROM fedora:41 AS builder

RUN printf '%s\n' \
  'fastestmirror=True' \
  'max_parallel_downloads=10' \
  'defaultyes=True' >> /etc/dnf/dnf.conf

# Install build dependencies
RUN dnf -y upgrade --refresh && dnf -y install \
  meson \
  ninja-build \
  gcc \
  gcc-c++ \
  cmake \
  git \
  pkgconf-pkg-config \
  # Vulkan
  vulkan-headers \
  vulkan-loader-devel \
  glslang \
  # Wayland
  wayland-devel \
  wayland-protocols-devel \
  # X11
  libX11-devel \
  libXdamage-devel \
  libXcomposite-devel \
  libXcursor-devel \
  libXrender-devel \
  libXext-devel \
  libXfixes-devel \
  libXxf86vm-devel \
  libXtst-devel \
  libXres-devel \
  libXmu-devel \
  libXi-devel \
  libxkbcommon-devel \
  # DRM/display
  libdrm-devel \
  pixman-devel \
  libdecor-devel \
  systemd-devel \
  libinput-devel \
  # PipeWire
  pipewire-devel \
  # Session/color
  libseat-devel \
  lcms2-devel \
  xorg-x11-server-Xwayland-devel \
  # Other deps
  SDL2-devel \
  libavif-devel \
  libcap-devel \
  libeis-devel \
  luajit-devel \
  hwdata-devel \
  # GLM / STB (system deps for bazzite fork)
  glm-devel \
  stb_image-devel \
  # For subprojects
  python3-jinja2 \
  && dnf clean all

# Additional wlroots XCB dependencies + libdisplay-info
RUN dnf -y install \
  xcb-util-wm-devel \
  xcb-util-errors-devel \
  xcb-util-renderutil-devel \
  libdisplay-info-devel \
  && dnf clean all

WORKDIR /build/gamescope

# Copy source
COPY . .

# Configure and build
RUN meson setup builddir \
  --prefix=/usr \
  --buildtype=release \
  --force-fallback-for=wlroots,libliftoff,vkroots \
  -Dpipewire=enabled \
  && ninja -C builddir

# Install to staging directory
RUN DESTDIR=/build/install ninja -C builddir install

# Output stage - just the built artifacts
FROM fedora:41
COPY --from=builder /build/install/ /
COPY --from=builder /build/gamescope/builddir/ /build/gamescope/builddir/
