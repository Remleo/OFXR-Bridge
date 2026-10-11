// XRFG_TEST_REPLAY=<sequence directory>: real frames recorded with
// XRFG_TEST_CAPTURE_FRAMES. Every method generates frame1 from frame0 and
// frame2 - the game's vectors doubled, as from frame2 to frame0 - and is
// compared with the real frame1: overall, and where frames 0 and 2 differ.

struct ReplayFrame {
    DXGI_FORMAT view_format{};
    std::vector<xrfg::D3D12ReprojectionView> views;
    xrfg::DumpedTexture colour;
    std::vector<xrfg::DlssMotionVectorFrame> guides;
    std::vector<std::string> motion_files, depth_files;
};

ReplayFrame load_replay_frame(const std::filesystem::path& folder) {
    ReplayFrame frame;
    std::ifstream meta(folder / "meta.txt");
    require(meta.good(), "replay: missing " + (folder / "meta.txt").string());
    std::string line;
    while (std::getline(meta, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        std::istringstream value(line.substr(equals + 1));
        if (key == "view_format") {
            int format = 0;
            value >> format;
            frame.view_format = static_cast<DXGI_FORMAT>(format);
        } else if (key.rfind("view", 0) == 0 && key != "view_count") {
            xrfg::D3D12ReprojectionView v{};
            value >> v.pose.orientation.x >> v.pose.orientation.y >> v.pose.orientation.z >>
                v.pose.orientation.w >> v.pose.position.x >> v.pose.position.y >> v.pose.position.z >>
                v.fov.angle_left >> v.fov.angle_right >> v.fov.angle_up >> v.fov.angle_down >>
                v.image_rect.offset_x >> v.image_rect.offset_y >> v.image_rect.width >>
                v.image_rect.height >> v.array_slice;
            frame.views.push_back(v);
        } else if (key.rfind("guide", 0) == 0 && key.find("_files") != std::string::npos) {
            std::string motion, depth;
            value >> motion >> depth;
            frame.motion_files.push_back(motion);
            frame.depth_files.push_back(depth);
        } else if (key.rfind("guide", 0) == 0) {
            xrfg::DlssMotionVectorFrame g{};
            unsigned long long stream = 0, epoch = 0, serial = 0, previous = 0;
            int jittered = 0, reset = 0, inverted = 0, infinite = 0;
            value >> stream >> epoch >> serial >> previous >> g.output_x >> g.output_y >>
                g.output_width >> g.output_height >> g.output_slice >> g.motion_x >> g.motion_y >>
                g.motion_width >> g.motion_height >> g.motion_slice >> g.scale_x >> g.scale_y >>
                g.jitter_x >> g.jitter_y >> g.previous_jitter_x >> g.previous_jitter_y >> jittered >>
                reset >> g.depth_x >> g.depth_y >> g.depth_width >> g.depth_height >>
                g.frame_time_delta_ms >> g.camera_near >> g.camera_far >> inverted >> infinite;
            g.stream = stream;
            g.epoch = epoch;
            g.jittered = jittered != 0;
            g.reset = reset != 0;
            g.depth_inverted = inverted != 0;
            g.depth_infinite = infinite != 0;
            frame.guides.push_back(g);
        }
    }
    require(xrfg::read_dumped_texture(folder / "colour.bin", &frame.colour), "replay: colour");
    // XRFG_TEST_REPLAY_SWAP_EYES=1 gives each view the other eye's guide.
    if (std::getenv("XRFG_TEST_REPLAY_SWAP_EYES") && frame.guides.size() == 2) {
        std::swap(frame.motion_files[0], frame.motion_files[1]);
        std::swap(frame.depth_files[0], frame.depth_files[1]);
        auto& a = frame.guides[0];
        auto& b = frame.guides[1];
        std::swap(a.motion_x, b.motion_x); std::swap(a.motion_y, b.motion_y);
        std::swap(a.motion_slice, b.motion_slice);
        std::swap(a.depth_x, b.depth_x); std::swap(a.depth_y, b.depth_y);
        std::swap(a.jitter_x, b.jitter_x); std::swap(a.jitter_y, b.jitter_y);
        std::swap(a.previous_jitter_x, b.previous_jitter_x);
        std::swap(a.previous_jitter_y, b.previous_jitter_y);
    }
    return frame;
}

ComPtr<ID3D12Resource> upload_dumped(D3D12WarpFixture& fixture, const xrfg::DumpedTexture& texture,
                                     D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES rest) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = texture.width;
    description.Height = texture.height;
    description.DepthOrArraySize = static_cast<UINT16>(texture.array_size);
    description.MipLevels = 1;
    description.Format = static_cast<DXGI_FORMAT>(texture.format);
    description.SampleDesc.Count = 1;
    description.Flags = flags;
    const D3D12_HEAP_PROPERTIES defaults = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> resource;
    require_hresult(fixture.device()->CreateCommittedResource(&defaults, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)),
        "replay texture");
    const UINT count = static_cast<UINT>(texture.subresources.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> row_bytes(count);
    UINT64 total = 0;
    fixture.device()->GetCopyableFootprints(&description, 0, count, 0, layouts.data(), rows.data(),
                                            row_bytes.data(), &total);
    auto upload = create_buffer(fixture, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    require_hresult(upload->Map(0, nullptr, &mapped), "replay upload map");
    for (UINT i = 0; i < count; ++i) {
        const auto& s = texture.subresources[i];
        for (UINT row = 0; row < rows[i]; ++row) {
            std::memcpy(static_cast<std::uint8_t*>(mapped) + layouts[i].Offset +
                            std::size_t(row) * layouts[i].Footprint.RowPitch,
                        s.data.data() + std::size_t(row) * s.row_bytes, s.row_bytes);
        }
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        for (UINT i = 0; i < count; ++i) {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = i;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[i];
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const auto barrier = transition_barrier(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, rest);
        list->ResourceBarrier(1, &barrier);
    });
    return resource;
}

xrfg::DumpedTexture read_back_texture(D3D12WarpFixture& fixture, ID3D12Resource* resource,
                                      D3D12_RESOURCE_STATES state) {
    const auto description = resource->GetDesc();
    const UINT count = description.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> row_bytes(count);
    UINT64 total = 0;
    fixture.device()->GetCopyableFootprints(&description, 0, count, 0, layouts.data(), rows.data(),
                                            row_bytes.data(), &total);
    auto readback = create_buffer(fixture, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        auto barrier = transition_barrier(resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        for (UINT i = 0; i < count; ++i) {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layouts[i];
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = resource;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = i;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        barrier = transition_barrier(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        list->ResourceBarrier(1, &barrier);
    });
    xrfg::DumpedTexture texture;
    texture.width = static_cast<std::uint32_t>(description.Width);
    texture.height = description.Height;
    texture.array_size = count;
    texture.format = description.Format;
    void* mapped = nullptr;
    require_hresult(readback->Map(0, nullptr, &mapped), "replay readback map");
    for (UINT i = 0; i < count; ++i) {
        xrfg::DumpedTexture::Subresource s;
        s.row_bytes = static_cast<std::uint32_t>(row_bytes[i]);
        s.rows = rows[i];
        s.data.resize(std::size_t(s.row_bytes) * s.rows);
        for (UINT row = 0; row < rows[i]; ++row) {
            std::memcpy(s.data.data() + std::size_t(row) * s.row_bytes,
                        static_cast<const std::uint8_t*>(mapped) + layouts[i].Offset +
                            std::size_t(row) * layouts[i].Footprint.RowPitch,
                        s.row_bytes);
        }
        texture.subresources.push_back(std::move(s));
    }
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return texture;
}

// A texel's colour on an 8-bit scale, for the formats swapchains use.
std::array<float, 3> replay_texel(const xrfg::DumpedTexture& t, UINT slice, UINT x, UINT y) {
    const auto& s = t.subresources[slice];
    const std::uint8_t* p = s.data.data() + std::size_t(y) * s.row_bytes;
    switch (static_cast<DXGI_FORMAT>(t.format)) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        std::uint32_t v = 0;
        std::memcpy(&v, p + std::size_t(x) * 4, 4);
        return {float(v & 1023) * 255 / 1023, float((v >> 10) & 1023) * 255 / 1023,
                float((v >> 20) & 1023) * 255 / 1023};
    }
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        std::array<float, 3> out{};
        for (UINT c = 0; c < 3; ++c) {
            std::uint16_t h = 0;
            std::memcpy(&h, p + std::size_t(x) * 8 + c * 2, 2);
            const int exponent = (h >> 10) & 31, mantissa = h & 1023;
            float value = exponent == 0 ? std::ldexp(float(mantissa), -24)
                                        : std::ldexp(float(mantissa | 1024), exponent - 25);
            if (h & 0x8000) value = -value;
            out[c] = std::clamp(value, 0.0F, 1.0F) * 255;
        }
        return out;
    }
    default:
        return {float(p[std::size_t(x) * 4]), float(p[std::size_t(x) * 4 + 1]),
                float(p[std::size_t(x) * 4 + 2])};
    }
}

void replay_held_out(const std::filesystem::path& directory) {
    D3D12WarpFixture fixture(true);
    // XRFG_TEST_REPLAY_FIRST picks the first of the three frames (0).
    const int first = std::getenv("XRFG_TEST_REPLAY_FIRST") ? std::atoi(std::getenv("XRFG_TEST_REPLAY_FIRST")) : 0;
    const auto frame_dir = [&](int i) { return directory / ("frame" + std::to_string(first + i)); };
    // XRFG_TEST_REPLAY_EXTRAPOLATE=1 predicts frame 2 from frames 0 and 1
    // instead: B is then frame 1, with its own vectors, and frame 2 the
    // truth. Below, f1 is always the truth and f2 always B.
    const bool extrapolate = std::getenv("XRFG_TEST_REPLAY_EXTRAPOLATE") != nullptr;
    const int b_index = extrapolate ? 1 : 2, truth_index = extrapolate ? 2 : 1;
    const ReplayFrame f0 = load_replay_frame(frame_dir(0));
    const ReplayFrame f1 = load_replay_frame(frame_dir(truth_index));
    const ReplayFrame f2 = load_replay_frame(frame_dir(b_index));
    // XRFG_TEST_REPLAY_GUIDE_SHIFT takes frame2's guides from a neighbouring
    // frame instead, to test whether colour and guides line up in time.
    const int guide_shift = std::getenv("XRFG_TEST_REPLAY_GUIDE_SHIFT")
        ? std::atoi(std::getenv("XRFG_TEST_REPLAY_GUIDE_SHIFT")) : 0;
    const ReplayFrame f2_guides = guide_shift ? load_replay_frame(frame_dir(b_index + guide_shift)) : f2;
    // XRFG_TEST_REPLAY_NATIVE_FIT_PREVIOUS=1 (with NATIVE_GPU_FIT) gives the
    // generating pair the fit of the pair before it, as live: the seeding
    // pair is then frame -1 to frame 0 with frame 0's own vectors, its fit is
    // read back, and the generating pair, which spans two frame times, gets
    // it scaled by the frame times (XRFG_TEST_NATIVE_FIT_SCALE=0: unscaled).
    const bool fit_previous = std::getenv("XRFG_TEST_REPLAY_NATIVE_FIT_PREVIOUS") != nullptr && first >= 1;
    const std::optional<ReplayFrame> f_previous =
        fit_previous ? std::optional<ReplayFrame>(load_replay_frame(frame_dir(-1))) : std::nullopt;
    const UINT width = f0.colour.width, height = f0.colour.height;
    const UINT slices = f0.colour.array_size;
    std::cout << "replay " << directory.string() << ": " << width << "x" << height << "x" << slices
              << " format " << f0.colour.format << ", " << f0.views.size() << " views, "
              << f2.guides.size() << " guides\n";
    const auto colour_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    // Frame times from DLSS: frame2's vectors reach frame1 in dt2, and frame0
    // is dt1 earlier still, so they are scaled to cover both, and frame1 lies
    // dt1 / (dt1 + dt2) of the way. Native generation is fixed at a half.
    const float dt1 = f1.guides.empty() ? 1.0F : f1.guides[0].frame_time_delta_ms;
    const float dt2 = f2.guides.empty() ? 1.0F : f2.guides[0].frame_time_delta_ms;
    // XRFG_TEST_REPLAY_VECTOR_GAIN multiplies that, to find what the vectors span.
    const float gain = std::getenv("XRFG_TEST_REPLAY_VECTOR_GAIN")
        ? float(std::atof(std::getenv("XRFG_TEST_REPLAY_VECTOR_GAIN"))) : 1.0F;
    // Extrapolating, B's vectors span exactly B to A, and the truth lies
    // dt1 past B: 1 + dt1 / dt2 in spans from A.
    const float vector_scale = extrapolate ? gain : gain * (dt2 > 0 ? (dt1 + dt2) / dt2 : 2.0F);
    // XRFG_TEST_REPLAY_FRACTION puts OFXR's frame elsewhere, such as at a
    // half like native generation's.
    const float fraction = std::getenv("XRFG_TEST_REPLAY_FRACTION")
        ? float(std::atof(std::getenv("XRFG_TEST_REPLAY_FRACTION")))
        : extrapolate ? 1.0F + (dt2 > 0 ? dt1 / dt2 : 1.0F)
        : dt1 + dt2 > 0 ? dt1 / (dt1 + dt2) : 0.5F;
    std::cout << "replay frame times " << dt1 << " and " << dt2 << " ms: fraction " << fraction
              << ", vectors x" << vector_scale << '\n';
    // XRFG_TEST_REPLAY_CHAIN_VECTORS=1 gives B the game's vectors chained from
    // frame2 to frame1 and on to frame0, rather than frame2's scaled by the
    // frame times. A head turn follows display time, not the game's, so where
    // frames are unevenly paced scaled vectors miss part of it; flow measures
    // the whole span. Where the chain leaves frame1, the scaled vector stands.
    // =head instead splits each vector: the tracked head turn from frame2 to
    // frame1, which the views give exactly, is replaced by the turn from
    // frame2 to frame0, and only the rest (the game's own motion) is scaled
    // by the frame times. Chaining samples frame1's vectors at moving edges,
    // where they belong to the wrong object.
    const char* chain_mode = extrapolate ? nullptr : std::getenv("XRFG_TEST_REPLAY_CHAIN_VECTORS");
    const bool chain = chain_mode != nullptr;
    const bool chain_head = chain && std::string_view(chain_mode) == "head";
    // Where pixel (x, y) of a width x height grid in `current`'s view lies in
    // `previous`'s, for a point at infinity: the displacement of a pure turn.
    const auto turn_displacement = [](const xrfg::D3D12ReprojectionView& current,
                                      const xrfg::D3D12ReprojectionView& previous, UINT width,
                                      UINT height, UINT x, UINT y) -> std::array<float, 2> {
        const auto rotate = [](const auto& q, std::array<float, 3> v, bool inverse) {
            const float qx = inverse ? -q.x : q.x, qy = inverse ? -q.y : q.y, qz = inverse ? -q.z : q.z;
            const float tx = 2 * (qy * v[2] - qz * v[1]), ty = 2 * (qz * v[0] - qx * v[2]),
                        tz = 2 * (qx * v[1] - qy * v[0]);
            return std::array<float, 3>{v[0] + q.w * tx + (qy * tz - qz * ty),
                                        v[1] + q.w * ty + (qz * tx - qx * tz),
                                        v[2] + q.w * tz + (qx * ty - qy * tx)};
        };
        const float l = std::tan(current.fov.angle_left), r = std::tan(current.fov.angle_right);
        const float u = std::tan(current.fov.angle_up), d = std::tan(current.fov.angle_down);
        const std::array<float, 3> ray{l + (float(x) + 0.5F) / float(width) * (r - l),
                                       u - (float(y) + 0.5F) / float(height) * (u - d), -1.0F};
        const auto there = rotate(previous.pose.orientation,
                                  rotate(current.pose.orientation, ray, false), true);
        if (there[2] >= -1e-4F) return {0.0F, 0.0F};
        const float pl = std::tan(previous.fov.angle_left), pr = std::tan(previous.fov.angle_right);
        const float pu = std::tan(previous.fov.angle_up), pd = std::tan(previous.fov.angle_down);
        const float px = there[0] / -there[2], py = there[1] / -there[2];
        return {(px - pl) / (pr - pl) * float(width) - 0.5F - float(x),
                (pu - py) / (pu - pd) * float(height) - 0.5F - float(y)};
    };
    const auto chained_motion = [&](std::size_t e) -> std::optional<xrfg::DumpedTexture> {
        if (!chain || e >= f1.guides.size() || e >= f2_guides.guides.size() ||
            f1.motion_files[e] == "-" || f2_guides.motion_files[e] == "-") return std::nullopt;
        if (chain_head && (e >= f0.views.size() || e >= f1.views.size() || e >= f2.views.size()))
            return std::nullopt;
        xrfg::DumpedTexture t2, t1;
        if (!xrfg::read_dumped_texture(frame_dir(b_index + guide_shift) / f2_guides.motion_files[e], &t2) ||
            !xrfg::read_dumped_texture(frame_dir(truth_index) / f1.motion_files[e], &t1) ||
            t2.format != DXGI_FORMAT_R16G16_FLOAT || t1.format != DXGI_FORMAT_R16G16_FLOAT)
            return std::nullopt;
        const auto half_to_float = [](std::uint16_t h) {
            const int exponent = (h >> 10) & 31, mantissa = h & 1023;
            float value = exponent == 0 ? std::ldexp(float(mantissa), -24)
                                        : std::ldexp(float(mantissa | 1024), exponent - 25);
            return (h & 0x8000) ? -value : value;
        };
        const auto float_to_half = [](float f) -> std::uint16_t {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, 4);
            const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000);
            const float magnitude = std::min(std::abs(f), 65504.0F);
            if (magnitude < 6.1035156e-05F)
                return static_cast<std::uint16_t>(sign | std::lround(magnitude / 5.9604645e-08F));
            int exponent = 0;
            const float fraction = std::frexp(magnitude, &exponent);  // [0.5, 1)
            long mantissa = std::lround((fraction * 2.0F - 1.0F) * 1024.0F);
            if (mantissa == 1024) { mantissa = 0; ++exponent; }
            return static_cast<std::uint16_t>(sign | ((exponent + 14) << 10) | mantissa);
        };
        const auto texel = [&](const xrfg::DumpedTexture& t, UINT slice, UINT x, UINT y) {
            const auto& s = t.subresources[slice];
            std::uint16_t h[2];
            std::memcpy(h, s.data.data() + std::size_t(y) * s.row_bytes + std::size_t(x) * 4, 4);
            return std::array<float, 2>{half_to_float(h[0]), half_to_float(h[1])};
        };
        const auto& g2 = f2_guides.guides[e];
        const auto& g1 = f1.guides[e];
        xrfg::DumpedTexture out = t2;
        auto& data = out.subresources[g2.motion_slice];
        for (UINT y = 0; y < g2.motion_height; ++y)
            for (UINT x = 0; x < g2.motion_width; ++x) {
                const auto v2 = texel(t2, g2.motion_slice, g2.motion_x + x, g2.motion_y + y);
                // In frame2's render pixels, then where that lands in frame1's.
                const float px = v2[0] * g2.scale_x, py = v2[1] * g2.scale_y;
                const long qx = std::lround(float(x) + px), qy = std::lround(float(y) + py);
                float cx = v2[0] * vector_scale, cy = v2[1] * vector_scale;
                if (chain_head) {
                    const auto to_frame1 = turn_displacement(f2.views[e], f1.views[e], g2.motion_width,
                                                        g2.motion_height, x, y);
                    const auto to_frame0 = turn_displacement(f2.views[e], f0.views[e], g2.motion_width,
                                                       g2.motion_height, x, y);
                    cx = (to_frame0[0] + (px - to_frame1[0]) * vector_scale) / g2.scale_x;
                    cy = (to_frame0[1] + (py - to_frame1[1]) * vector_scale) / g2.scale_y;
                } else if (qx >= 0 && qy >= 0 && qx < long(g1.motion_width) && qy < long(g1.motion_height)) {
                    const auto v1 = texel(t1, g1.motion_slice, g1.motion_x + UINT(qx), g1.motion_y + UINT(qy));
                    cx = (px + v1[0] * g1.scale_x) / g2.scale_x;
                    cy = (py + v1[1] * g1.scale_y) / g2.scale_y;
                }
                const std::uint16_t h[2] = {float_to_half(cx), float_to_half(cy)};
                std::memcpy(data.data.data() + std::size_t(g2.motion_y + y) * data.row_bytes +
                                std::size_t(g2.motion_x + x) * 4, h, 4);
            }
        return out;
    };
    // Guides as the layer hands them on: A's, and B's scaled to reach A.
    const auto make_guides = [&](const ReplayFrame& frame, std::uint64_t serial, float scale,
                                 const ReplayFrame* previous) {
        auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
        set->eye_count = static_cast<std::uint32_t>(frame.guides.size());
        std::map<std::string, ComPtr<ID3D12Resource>> uploaded;
        for (std::size_t e = 0; e < frame.guides.size(); ++e) {
            auto g = std::make_shared<xrfg::DlssMotionVectorFrame>(frame.guides[e]);
            g->stream = f2.guides[e].stream;
            g->epoch = f2.guides[e].epoch;
            g->serial = serial;
            g->previous_serial = serial - 1;
            g->scale_x *= scale;
            g->scale_y *= scale;
            // Vectors scaled to reach A span that much more time as well.
            g->frame_time_delta_ms = frame.guides[e].frame_time_delta_ms * scale;
            if (previous) {
                g->previous_jitter_x = previous->guides[e].jitter_x;
                g->previous_jitter_y = previous->guides[e].jitter_y;
            }
            g->reset = false;
            g->producer_queue = fixture.queue();
            const auto load = [&](const std::string& name) -> ComPtr<ID3D12Resource> {
                if (name == "-") return {};
                auto found = uploaded.find(name);
                if (found != uploaded.end()) return found->second;
                xrfg::DumpedTexture t;
                const int guide_frame = &frame == &f0 ? 0
                    : (f_previous && &frame == &*f_previous) ? -1 : b_index + guide_shift;
                require(xrfg::read_dumped_texture(frame_dir(guide_frame) / name, &t), "replay: guide " + name);
                auto r = upload_dumped(fixture, t, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                uploaded[name] = r;
                return r;
            };
            const auto chained = &frame == &f2_guides ? chained_motion(e) : std::nullopt;
            if (chained) {
                g->scale_x = frame.guides[e].scale_x;
                g->scale_y = frame.guides[e].scale_y;
                g->motion_vectors = upload_dumped(fixture, *chained, D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            } else {
                g->motion_vectors = load(frame.motion_files[e]);
            }
            g->depth = load(frame.depth_files[e]);
            g->resource_state = g->depth_resource_state =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            set->eyes[e] = g;
        }
        return set;
    };
    const auto guides_a = make_guides(f0, 1, 1.0F, nullptr);
    const auto guides_b = make_guides(f2_guides, 2, vector_scale, &f0);
    // Runs body(first, end, thread) over [0, count) on every core.
    const auto parallel = [](UINT count, const auto& body) {
        const UINT threads = std::max(1U, std::min(count, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (UINT i = 0; i < threads; ++i)
            pool.emplace_back([&, i] { body(count * i / threads, count * (i + 1) / threads, i); });
        for (auto& thread : pool) thread.join();
        return threads;
    };
    // An image as RGB floats, one row of the array after another.
    const auto rgb = [&](const xrfg::DumpedTexture& image) {
        std::vector<float> out(std::size_t(width) * height * slices * 3);
        parallel(height * slices, [&](UINT first, UINT end, UINT) {
            for (UINT row = first; row < end; ++row)
                for (UINT x = 0; x < width; ++x) {
                    const auto c = replay_texel(image, row / height, x, row % height);
                    for (UINT k = 0; k < 3; ++k) out[(std::size_t(row) * width + x) * 3 + k] = c[k];
                }
        });
        return out;
    };
    const std::vector<float> rgb0 = rgb(extrapolate ? f1.colour : f0.colour), rgb2 = rgb(f2.colour);
    // Where the scene moved between frames 0 and 2 (extrapolating, between
    // B and the truth).
    std::vector<std::uint8_t> moving(std::size_t(width) * height * slices, 0);
    for (std::size_t i = 0; i < moving.size(); ++i) {
        float d = 0;
        for (UINT c = 0; c < 3; ++c) d = std::max(d, std::abs(rgb0[i * 3 + c] - rgb2[i * 3 + c]));
        moving[i] = d > 6;
    }
    // XRFG_TEST_REPLAY_SAVE=<dir> writes each image's crop around the motion.
    const char* save_dir = std::getenv("XRFG_TEST_REPLAY_SAVE");
    UINT box_left = width, box_top = height, box_right = 0, box_bottom = 0, box_slice = 0;
    for (UINT y = 8; y + 8 < height; ++y)
        for (UINT x = 8; x + 8 < width; ++x)
            if (moving[std::size_t(y) * width + x]) {
                box_left = std::min(box_left, x); box_right = std::max(box_right, x);
                box_top = std::min(box_top, y); box_bottom = std::max(box_bottom, y);
            }
    // XRFG_TEST_REPLAY_SAVE_FULL=1 saves whole images instead.
    if (std::getenv("XRFG_TEST_REPLAY_SAVE_FULL")) {
        box_left = 0; box_top = 0; box_right = width - 1; box_bottom = height - 1;
    } else if (box_left < box_right) {
        box_left = box_left > 32 ? box_left - 32 : 0;
        box_top = box_top > 32 ? box_top - 32 : 0;
        box_right = std::min(std::min(box_right + 32, width - 1), box_left + 1023);
        box_bottom = std::min(std::min(box_bottom + 32, height - 1), box_top + 1023);
    }
    const auto save = [&](const std::string& name, const xrfg::DumpedTexture& image) {
        if (!save_dir || box_left >= box_right) return;
        std::filesystem::create_directories(save_dir);
        std::string file = name;
        for (auto& c : file) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        std::ofstream out(std::filesystem::path(save_dir) / (file + ".ppm"), std::ios::binary);
        out << "P6\n" << (box_right - box_left + 1) << " " << (box_bottom - box_top + 1) << "\n255\n";
        for (UINT y = box_top; y <= box_bottom; ++y)
            for (UINT x = box_left; x <= box_right; ++x) {
                const auto c = replay_texel(image, box_slice, x, y);
                for (UINT k = 0; k < 3; ++k) out.put(static_cast<char>(std::lround(std::clamp(c[k], 0.0F, 255.0F))));
            }
    };
    save("0 frame0", f0.colour);
    save("1 truth frame1", f1.colour);
    save("2 frame2", f2.colour);
    // Luma, for the structural scores below.
    const auto luma = [&](const std::vector<float>& image) {
        std::vector<float> out(image.size() / 3);
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = 0.299F * image[i * 3] + 0.587F * image[i * 3 + 1] + 0.114F * image[i * 3 + 2];
        return out;
    };
    const std::vector<float> truth_rgb = rgb(f1.colour);
    const std::vector<float> truth_luma = luma(truth_rgb);
    const auto score = [&](const char* name, const xrfg::DumpedTexture& actual) {
        save(std::string("m ") + name, actual);
        const std::vector<float> out_rgb = rgb(actual);
        const std::vector<float> out_luma = luma(out_rgb);
        // Per thread: absolute error everywhere and where the scene moved,
        // their counts, and the structural sums described below.
        struct Sums {
            double all = 0, moved = 0, ssim = 0, gradient_out = 0, gradient_truth = 0, gradient_error = 0;
            std::size_t all_count = 0, moved_count = 0, blocks = 0, gradients = 0;
            // Where the scene moved, per eye: each half of a side-by-side
            // image, or each slice of an array.
            double eye_moved[2]{};
            std::size_t eye_moved_count[2]{};
            // XRFG_TEST_REPLAY_REGION=eye,x0,y0,x1,y1 (fractions of that
            // eye): the error over that window alone, moving or not, for
            // a character the whole-frame numbers cannot see.
            double region = 0;
            std::size_t region_count = 0;
        };
        std::vector<Sums> sums(std::thread::hardware_concurrency() + 1);
        static const auto region = [] {
            std::array<float, 5> r{-1.0F, 0.0F, 0.0F, 0.0F, 0.0F};
            if (const char* text = std::getenv("XRFG_TEST_REPLAY_REGION")) {
                std::sscanf(text, "%f,%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3], &r[4]);
            }
            return r;
        }();
        const UINT rows = height * slices;
        parallel(rows, [&](UINT first, UINT end, UINT thread) {
            Sums& sum = sums[thread];
            for (UINT row = first; row < end; ++row) {
                const UINT y = row % height;
                if (y < 8 || y + 8 >= height) continue;
                for (UINT x = 8; x + 8 < width; ++x) {
                    const std::size_t i = std::size_t(row) * width + x;
                    double e = 0;
                    for (UINT c = 0; c < 3; ++c) e += std::abs(out_rgb[i * 3 + c] - truth_rgb[i * 3 + c]);
                    sum.all += e;
                    sum.all_count += 3;
                    if (region[0] >= 0.0F) {
                        const UINT eye_of = slices > 1 ? std::min<UINT>(row / height, 1) : (x >= width / 2 ? 1 : 0);
                        const UINT eye_width = slices > 1 ? width : width / 2;
                        const float fx = float(x - (slices > 1 ? 0 : eye_of * eye_width)) / float(eye_width);
                        const float fy = float(y) / float(height);
                        if (UINT(region[0]) == eye_of && fx >= region[1] && fy >= region[2] && fx < region[3] && fy < region[4]) {
                            sum.region += e;
                            sum.region_count += 3;
                        }
                    }
                    if (!moving[i]) continue;
                    sum.moved += e;
                    sum.moved_count += 3;
                    const UINT eye = slices > 1 ? std::min<UINT>(row / height, 1) : (x >= width / 2 ? 1 : 0);
                    sum.eye_moved[eye] += e;
                    sum.eye_moved_count[eye] += 3;
                    // The output's gradient against the truth's (below 1 is
                    // blurrier) and their mean difference, which a blur and a
                    // misplaced edge both raise.
                    const float* a = out_luma.data();
                    const float* tr = truth_luma.data();
                    const double gax = a[i + 1] - a[i - 1], gay = a[i + width] - a[i - width];
                    const double gtx = tr[i + 1] - tr[i - 1], gty = tr[i + width] - tr[i - width];
                    sum.gradient_out += std::sqrt(gax * gax + gay * gay);
                    sum.gradient_truth += std::sqrt(gtx * gtx + gty * gty);
                    sum.gradient_error += std::sqrt((gax - gtx) * (gax - gtx) + (gay - gty) * (gay - gty));
                    ++sum.gradients;
                }
                // SSIM over the 8x8 luma blocks starting on this row, where
                // most of the block moved.
                if (y % 8 != 0 || y + 16 > height) continue;
                constexpr double c1 = 6.5025, c2 = 58.5225;
                for (UINT bx = 8; bx + 16 <= width; bx += 8) {
                    double ma = 0, mt = 0, aa = 0, tt = 0, at = 0;
                    int count = 0;
                    for (UINT by = 0; by < 8; ++by)
                        for (UINT x = bx; x < bx + 8; ++x) {
                            const std::size_t i = (std::size_t(row) + by) * width + x;
                            const double va = out_luma[i], vt = truth_luma[i];
                            ma += va; mt += vt; aa += va * va; tt += vt * vt; at += va * vt;
                            count += moving[i];
                        }
                    if (count < 32) continue;
                    ma /= 64; mt /= 64;
                    const double va = aa / 64 - ma * ma, vt = tt / 64 - mt * mt, cov = at / 64 - ma * mt;
                    sum.ssim += ((2 * ma * mt + c1) * (2 * cov + c2)) / ((ma * ma + mt * mt + c1) * (va + vt + c2));
                    ++sum.blocks;
                }
            }
        });
        Sums total;
        for (const auto& s : sums) {
            total.all += s.all; total.moved += s.moved; total.ssim += s.ssim;
            total.gradient_out += s.gradient_out; total.gradient_truth += s.gradient_truth;
            total.gradient_error += s.gradient_error;
            total.all_count += s.all_count; total.moved_count += s.moved_count;
            total.blocks += s.blocks; total.gradients += s.gradients;
            for (int eye = 0; eye < 2; ++eye) {
                total.eye_moved[eye] += s.eye_moved[eye];
                total.eye_moved_count[eye] += s.eye_moved_count[eye];
            }
            total.region += s.region;
            total.region_count += s.region_count;
        }
        std::cout << "replay " << name << " mae=" << total.all / double(total.all_count)
                  << " moving=" << (total.moved_count ? total.moved / double(total.moved_count) : 0.0)
                  << " moving_fraction=" << double(total.moved_count) / double(total.all_count)
                  << " ssim=" << (total.blocks ? total.ssim / double(total.blocks) : 1.0)
                  << " sharpness=" << (total.gradient_truth > 0 ? total.gradient_out / total.gradient_truth : 1.0)
                  << " gradient_error=" << (total.gradients ? total.gradient_error / double(total.gradients) : 0.0)
                  << " eye_moving=" << (total.eye_moved_count[0] ? total.eye_moved[0] / double(total.eye_moved_count[0]) : 0.0)
                  << ',' << (total.eye_moved_count[1] ? total.eye_moved[1] / double(total.eye_moved_count[1]) : 0.0);
        if (total.region_count) std::cout << " region=" << total.region / double(total.region_count);
        std::cout << '\n';
    };
    score("repeat frame2", f2.colour);
    {
        xrfg::DumpedTexture blend = f2.colour;
        for (UINT s = 0; s < slices; ++s)
            for (std::size_t i = 0; i < blend.subresources[s].data.size(); ++i)
                blend.subresources[s].data[i] = static_cast<std::uint8_t>(
                    (int(f0.colour.subresources[s].data[i]) + f2.colour.subresources[s].data[i] + 1) / 2);
        const auto f = static_cast<DXGI_FORMAT>(f0.colour.format);
        if (f != DXGI_FORMAT_R10G10B10A2_UNORM && f != DXGI_FORMAT_R10G10B10A2_TYPELESS &&
            f != DXGI_FORMAT_R16G16B16A16_FLOAT && f != DXGI_FORMAT_R16G16B16A16_TYPELESS)
            score("blend", blend);
    }
    const std::span<const xrfg::D3D12ReprojectionView> views0(f0.views), views1(f1.views), views2(f2.views);
    struct Method {
        std::string name;
        xrfg::D3D12OpticalFlowBackend backend;
        xrfg::D3D12OpticalFlowInputScale scale;
        bool guides;
        xrfg::D3D12NvidiaPerformancePreset preset{xrfg::D3D12NvidiaPerformancePreset::medium};
        bool bidirectional{};
        bool hybrid{};
        bool extrapolate{};
        bool extrapolate_hybrid{};
        bool extrapolate_mesh{};
    };
    std::vector<Method> methods{
        {"OFXR+DLSS vectors", xrfg::D3D12OpticalFlowBackend::fidelity_fx,
         xrfg::D3D12OpticalFlowInputScale::half, true},
        {"OFXR FidelityFX half-res flow", xrfg::D3D12OpticalFlowBackend::fidelity_fx,
         xrfg::D3D12OpticalFlowInputScale::half, false},
        {"OFXR FidelityFX full-res flow", xrfg::D3D12OpticalFlowBackend::fidelity_fx,
         xrfg::D3D12OpticalFlowInputScale::full, false},
        {"OFXR NVIDIA medium flow", xrfg::D3D12OpticalFlowBackend::nvidia,
         xrfg::D3D12OpticalFlowInputScale::half, false}};
    // XRFG_TEST_REPLAY_FLOWS lists OFXR configurations to run instead, each
    // backend/preset/scale[/bi]: vectors, hybrid (vectors and FidelityFX
    // flow), extrapolate (from the game's vectors and depth, for
    // XRFG_TEST_REPLAY_EXTRAPOLATE), ffx or nvidia; slow, medium or fast;
    // full, three_quarter or half. For example nvidia/fast/full/bi.
    if (const char* list = std::getenv("XRFG_TEST_REPLAY_FLOWS")) {
        methods.clear();
        std::stringstream stream(list);
        for (std::string item; std::getline(stream, item, ',');) {
            std::vector<std::string> parts;
            std::stringstream fields(item);
            for (std::string part; std::getline(fields, part, '/');) parts.push_back(part);
            require(parts.size() >= 3, "replay: flow " + item);
            Method m{item, parts[0] == "nvidia" ? xrfg::D3D12OpticalFlowBackend::nvidia
                                                : xrfg::D3D12OpticalFlowBackend::fidelity_fx,
                     parts[2] == "full"            ? xrfg::D3D12OpticalFlowInputScale::full
                     : parts[2] == "three_quarter" ? xrfg::D3D12OpticalFlowInputScale::three_quarter
                     : parts[2] == "quarter"       ? xrfg::D3D12OpticalFlowInputScale::quarter
                                                   : xrfg::D3D12OpticalFlowInputScale::half,
                     parts[0] == "vectors" || parts[0] == "hybrid"};
            m.hybrid = parts[0] == "hybrid";
            // extrapolate_flow extrapolates from FidelityFX's flow, without
            // guides, and extrapolate_hybrid from both.
            // extrapolate_mesh and extrapolate_flow_mesh are Meta's mesh
            // warps: Application SpaceWarp's from the vectors and depth,
            // Asynchronous SpaceWarp's from the flow.
            m.extrapolate_mesh = parts[0] == "extrapolate_mesh" || parts[0] == "extrapolate_flow_mesh" ||
                                 parts[0] == "extrapolate_hybrid_mesh";
            m.extrapolate = parts[0] == "extrapolate" || parts[0] == "extrapolate_flow" ||
                            parts[0] == "extrapolate_hybrid" || m.extrapolate_mesh;
            m.extrapolate_hybrid = parts[0] == "extrapolate_hybrid" || parts[0] == "extrapolate_hybrid_mesh";
            m.guides = m.guides || parts[0] == "extrapolate" || parts[0] == "extrapolate_hybrid" ||
                       parts[0] == "extrapolate_mesh" || parts[0] == "extrapolate_hybrid_mesh";
            m.preset = parts[1] == "slow"   ? xrfg::D3D12NvidiaPerformancePreset::slow
                       : parts[1] == "fast" ? xrfg::D3D12NvidiaPerformancePreset::fast
                                            : xrfg::D3D12NvidiaPerformancePreset::medium;
            m.bidirectional = parts.size() > 3 && parts[3] == "bi";
            methods.push_back(m);
        }
    }
    // XRFG_TEST_REPLAY_NATIVE_ONLY=1 skips OFXR's methods;
    // XRFG_TEST_REPLAY_NATIVE_SCALES lists native resolutions (100,67).
    const bool native_only = std::getenv("XRFG_TEST_REPLAY_NATIVE_ONLY") != nullptr;
    for (const auto& m : methods) {
        if (native_only) break;
        // XRFG_TEST_REPLAY_VECTORS_ONLY=1 runs only OFXR + DLSS vectors.
        if (std::getenv("XRFG_TEST_REPLAY_VECTORS_ONLY") && !m.guides) continue;
        auto a = upload_dumped(fixture, f0.colour, colour_flags, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto b = upload_dumped(fixture, f2.colour, colour_flags, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto blank = [&] {
            return create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                         static_cast<UINT16>(slices), static_cast<DXGI_FORMAT>(f0.colour.format));
        };
        std::array<ComPtr<ID3D12Resource>, 2> currents{blank(), blank()};
        auto synthetic = blank();
        std::array<ID3D12Resource*, 2> sources{a.Get(), b.Get()};
        std::array<ID3D12Resource*, 2> current_pointers{currents[0].Get(), currents[1].Get()};
        std::array<ID3D12Resource*, 1> synthetic_pointers{synthetic.Get()};
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(), sources,
                    D3D12_RESOURCE_STATE_RENDER_TARGET)), "replay history");
        xrfg::D3D12FrameSynthesizer synthesizer;
        xrfg::D3D12NvidiaOpticalFlowOptions options;
        options.input_scale = m.scale;
        options.preset = m.preset;
        options.bidirectional = m.bidirectional;
        options.hybrid = m.hybrid;
        options.extrapolate = m.extrapolate;
        options.extrapolate_hybrid = m.extrapolate_hybrid;
        options.extrapolate_mesh = m.extrapolate_mesh;
        require(operation_succeeded(synthesizer.initialize(fixture.device(), fixture.queue(), history,
                    current_pointers, synthetic_pointers, f0.view_format,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, m.backend, options, true)), "replay synthesizer");
        xrfg::D3D12HistoryCaptureTicket capture_a{}, capture_b{};
        require(operation_succeeded(history->capture(0, &capture_a)) &&
                    operation_succeeded(history->commit(capture_a)), "replay capture A");
        xrfg::D3D12FrameSynthesisTicket prime{}, pair{};
        require(operation_succeeded(synthesizer.submit_prime(capture_a, views0, 0, &prime,
                    m.guides ? guides_a : nullptr)), "replay prime");
        require_frame_start_gate(synthesizer, "replay frame-start gate");
        require(operation_succeeded(history->capture(1, &capture_b)) &&
                    operation_succeeded(history->commit(capture_b)), "replay capture B");
        require(operation_succeeded(synthesizer.submit_pair(capture_b, views2, views1, 0, 1, &pair,
                    std::nullopt, m.guides ? guides_b : nullptr, false, fraction)), "replay pair");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
        // The pair's GPU span on real content. One pair is cold; with
        // XRFG_TEST_REPLAY_TIMING_PAIRS=n the prime and pair are run n times
        // more after scoring and the median of those is printed as well.
        const auto pair_microseconds = [&]() -> double {
            xrfg::D3D12NvidiaGpuTiming timing{};
            if (synthesizer.consume_nvidia_gpu_timing(&timing) != S_OK) return -1.0;
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            return timing.gpu_end_qpc > timing.gpu_begin_qpc
                ? double(timing.gpu_end_qpc - timing.gpu_begin_qpc) * 1e6 / double(frequency.QuadPart)
                : double(timing.total_microseconds);
        };
        if (const double microseconds = pair_microseconds(); microseconds >= 0.0) {
            std::cout << "replay " << m.name << " gpu_us=" << microseconds << std::endl;
        }
        score(m.name.c_str(), read_back_texture(fixture, synthetic.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET));
        if (const char* repeat = std::getenv("XRFG_TEST_REPLAY_TIMING_PAIRS")) {
            std::vector<double> samples;
            for (int run = 0; run < std::atoi(repeat); ++run) {
                xrfg::D3D12HistoryCaptureTicket again_a{}, again_b{};
                xrfg::D3D12FrameSynthesisTicket again_prime{}, again_pair{};
                if (!operation_succeeded(synthesizer.wait_for_idle()) ||
                    !operation_succeeded(history->capture(0, &again_a)) ||
                    !operation_succeeded(history->commit(again_a)) ||
                    !operation_succeeded(synthesizer.submit_prime(again_a, views0, 0, &again_prime,
                        m.guides ? guides_a : nullptr))) break;
                require_frame_start_gate(synthesizer, "replay timing frame-start gate");
                if (!operation_succeeded(history->capture(1, &again_b)) ||
                    !operation_succeeded(history->commit(again_b)) ||
                    !operation_succeeded(synthesizer.submit_pair(again_b, views2, views1, 0, 1, &again_pair,
                        std::nullopt, m.guides ? guides_b : nullptr, false, fraction))) break;
                fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
                if (const double microseconds = pair_microseconds(); microseconds >= 0.0) {
                    samples.push_back(microseconds);
                }
            }
            if (!samples.empty()) {
                std::sort(samples.begin(), samples.end());
                std::cout << "replay " << m.name << " warm_gpu_us=" << samples[samples.size() / 2]
                          << " runs=" << samples.size() << std::endl;
            }
        }
        require(operation_succeeded(synthesizer.wait_for_idle()), "replay drain");
    }
#ifdef XRFG_NATIVE_DLSSG
    std::vector<std::string> native_scales{"100", "67"};
    if (const char* list = std::getenv("XRFG_TEST_REPLAY_NATIVE_SCALES")) {
        native_scales.clear();
        std::stringstream stream(list);
        for (std::string item; std::getline(stream, item, ',');) native_scales.push_back(item);
    }
    if (extrapolate || std::getenv("XRFG_TEST_REPLAY_VECTORS_ONLY") ||
        (std::getenv("XRFG_TEST_REPLAY_FLOWS") && !std::getenv("XRFG_TEST_REPLAY_NATIVE_SCALES")))
        native_scales.clear();
    // XRFG_TEST_REPLAY_NATIVE_GPU_FIT=1 runs the live camera-motion path: the
    // GPU fits the camera's motion to the generating pair, and the pair is
    // generated again with it, as the next live pair would be.
    const bool gpu_fit = std::getenv("XRFG_TEST_REPLAY_NATIVE_GPU_FIT") != nullptr;
    if (gpu_fit) SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_CAMERA", "1");
    for (const auto& scale_text : native_scales) {
        const char* scale = scale_text.c_str();
        SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_SCALE", scale);
        auto a = upload_dumped(fixture, f0.colour, colour_flags, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto b = upload_dumped(fixture, f2.colour, colour_flags, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto output = create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            static_cast<UINT16>(slices), static_cast<DXGI_FORMAT>(f0.colour.format));
        // Generated in frame 1's camera, as OFXR's methods are above, so a head
        // turn between the frames is not scored against native as error.
        // XRFG_TEST_REPLAY_NATIVE_B_CAMERA=1 keeps B's camera instead.
        std::array<xrfg::D3D12NativeDlssG::Output, 1> outputs{{{output.Get()}}};
        // XRFG_TEST_REPLAY_NATIVE_UNALIGNED=1 hands NGX A as it is and the
        // game's whole vectors, head turn included, with B's views for both:
        // nothing is aligned or reseeded, and NGX's frame lies midway between
        // the two cameras, from where it is turned into frame 1's.
        const bool unaligned = std::getenv("XRFG_TEST_REPLAY_NATIVE_UNALIGNED") != nullptr;
        std::vector<xrfg::D3D12ReprojectionView> midpoint(views2.begin(), views2.end());
        for (std::size_t i = 0; unaligned && i < midpoint.size() && i < views0.size(); ++i) {
            const auto& q0 = views0[i].pose.orientation;
            auto& q = midpoint[i].pose.orientation;
            const float sign = q0.x * q.x + q0.y * q.y + q0.z * q.z + q0.w * q.w < 0 ? -1.0F : 1.0F;
            q.x = q0.x + sign * q.x; q.y = q0.y + sign * q.y; q.z = q0.z + sign * q.z; q.w = q0.w + sign * q.w;
            const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            q.x /= n; q.y /= n; q.z /= n; q.w /= n;
        }
        std::span<const xrfg::D3D12ReprojectionView> native_a = unaligned ? views2 : views0;
        // XRFG_TEST_REPLAY_NATIVE_GAME_ROTATION=1 aligns A by the whole turn
        // between the frames, the game camera's included, fitted to B's
        // vectors: a rotation taking B's view rays to where the vectors put
        // their content in A. A is given the orientation that makes it the
        // alignment's, and the generated frame is turned back by half of it.
        std::vector<xrfg::D3D12ReprojectionView> rotated_a(views0.begin(), views0.end());
        std::vector<xrfg::D3D12ReprojectionView> rotated_target(views1.begin(), views1.end());
        const bool game_rotation = std::getenv("XRFG_TEST_REPLAY_NATIVE_GAME_ROTATION") != nullptr &&
            !unaligned && views0.size() == views2.size() && views1.size() == views2.size();
        if (game_rotation) {
            using Q = xrfg::Quaternion;
            const auto mul = [](const Q& a, const Q& b) {
                return Q{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                         a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
            };
            const auto conj = [](const Q& q) { return Q{-q.x, -q.y, -q.z, q.w}; };
            const auto apply = [&](const Q& q, std::array<double, 3> v) {
                const Q p{float(v[0]), float(v[1]), float(v[2]), 0.0F};
                const Q r = mul(mul(q, p), conj(q));
                return std::array<double, 3>{r.x, r.y, r.z};
            };
            const auto half_to_float = [](std::uint16_t h) {
                const int exponent = (h >> 10) & 31, mantissa = h & 1023;
                float value = exponent == 0 ? std::ldexp(float(mantissa), -24)
                                            : std::ldexp(float(mantissa | 1024), exponent - 25);
                return (h & 0x8000) ? -value : value;
            };
            for (std::size_t e = 0; e < views2.size() && e < f2_guides.guides.size(); ++e) {
                xrfg::DumpedTexture motion;
                if (f2_guides.motion_files[e] == "-" ||
                    !xrfg::read_dumped_texture(frame_dir(b_index + guide_shift) / f2_guides.motion_files[e], &motion) ||
                    (motion.format != DXGI_FORMAT_R16G16_FLOAT && motion.format != DXGI_FORMAT_R32G32_FLOAT)) continue;
                const auto& g = f2_guides.guides[e];
                const auto& vb = views2[e];
                const auto& va = views0[e];
                const auto texel = [&](UINT x, UINT y) {
                    const auto& s = motion.subresources[g.motion_slice];
                    const std::uint8_t* p = s.data.data() + std::size_t(y) * s.row_bytes;
                    if (motion.format == DXGI_FORMAT_R32G32_FLOAT) {
                        float v[2];
                        std::memcpy(v, p + std::size_t(x) * 8, 8);
                        return std::array<float, 2>{v[0], v[1]};
                    }
                    std::uint16_t h[2];
                    std::memcpy(h, p + std::size_t(x) * 4, 4);
                    return std::array<float, 2>{half_to_float(h[0]), half_to_float(h[1])};
                };
                const auto ray = [](const xrfg::D3D12FieldOfView& f, double u, double v) {
                    const double l = std::tan(f.angle_left), r = std::tan(f.angle_right);
                    const double up = std::tan(f.angle_up), down = std::tan(f.angle_down);
                    std::array<double, 3> d{l + u * (r - l), up - v * (up - down), -1.0};
                    const double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    return std::array<double, 3>{d[0] / n, d[1] / n, d[2] / n};
                };
                // B's ray and the ray of where its content was in A.
                std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>> pairs;
                constexpr UINT kGrid = 48;
                for (UINT j = 0; j < kGrid; ++j)
                    for (UINT i = 0; i < kGrid; ++i) {
                        const double u = (i + 0.5) / kGrid, v = (j + 0.5) / kGrid;
                        const auto m = texel(g.motion_x + UINT(u * g.motion_width), g.motion_y + UINT(v * g.motion_height));
                        const double du = double(m[0]) * g.scale_x * vector_scale / g.motion_width;
                        const double dv = double(m[1]) * g.scale_y * vector_scale / g.motion_height;
                        if (!std::isfinite(du) || !std::isfinite(dv) || std::abs(du) > 1 || std::abs(dv) > 1) continue;
                        pairs.emplace_back(ray(vb.fov, u, v), ray(va.fov, u + du, v + dv));
                    }
                if (pairs.size() < 64) continue;
                // Gauss-Newton on the rotation, dropping the worst fitting
                // points (objects moving on their own) as it goes.
                Q r{0, 0, 0, 1};
                std::vector<std::uint8_t> use(pairs.size(), 1);
                for (int pass = 0; pass < 6; ++pass) {
                    double h[3][3]{}, b[3]{};
                    std::vector<double> residual(pairs.size(), 0.0);
                    for (std::size_t k = 0; k < pairs.size(); ++k) {
                        const auto s = apply(r, pairs[k].first);
                        const auto& t = pairs[k].second;
                        const std::array<double, 3> err{t[0] - s[0], t[1] - s[1], t[2] - s[2]};
                        residual[k] = std::sqrt(err[0] * err[0] + err[1] * err[1] + err[2] * err[2]);
                        if (!use[k]) continue;
                        const double ss = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
                        for (int a = 0; a < 3; ++a)
                            for (int c = 0; c < 3; ++c) h[a][c] += (a == c ? ss : 0.0) - s[a] * s[c];
                        b[0] += s[1] * err[2] - s[2] * err[1];
                        b[1] += s[2] * err[0] - s[0] * err[2];
                        b[2] += s[0] * err[1] - s[1] * err[0];
                    }
                    const double det = h[0][0] * (h[1][1] * h[2][2] - h[1][2] * h[2][1]) -
                                       h[0][1] * (h[1][0] * h[2][2] - h[1][2] * h[2][0]) +
                                       h[0][2] * (h[1][0] * h[2][1] - h[1][1] * h[2][0]);
                    if (std::abs(det) < 1e-12) break;
                    double x[3];
                    for (int col = 0; col < 3; ++col) {
                        double m[3][3];
                        for (int a = 0; a < 3; ++a)
                            for (int c = 0; c < 3; ++c) m[a][c] = c == col ? b[a] : h[a][c];
                        x[col] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                                  m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                                  m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
                    }
                    const double angle = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                    const double sh = angle > 1e-12 ? std::sin(angle / 2) / angle : 0.5;
                    const Q step{float(x[0] * sh), float(x[1] * sh), float(x[2] * sh), float(std::cos(angle / 2))};
                    r = mul(step, r);
                    // Keep the closer three quarters of the points for the next pass.
                    std::vector<double> sorted(residual);
                    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() * 3 / 4, sorted.end());
                    const double cut = sorted[sorted.size() * 3 / 4];
                    for (std::size_t k = 0; k < pairs.size(); ++k) use[k] = residual[k] <= cut;
                }
                if (r.w < 0) r = Q{-r.x, -r.y, -r.z, -r.w};
                // b_to_a(a', b) = conj(qA') qB = r, so qA' = qB conj(r).
                const Q qb = vb.pose.orientation;
                rotated_a[e].pose.orientation = mul(qb, conj(r));
                // Half the turn back from B: the camera of NGX's midpoint.
                const float half_w = std::sqrt(std::max(0.0F, (1.0F + r.w) / 2.0F));
                const float scale = half_w > 1e-6F ? 0.5F / half_w : 0.0F;
                const Q half{r.x * scale, r.y * scale, r.z * scale, half_w};
                rotated_target[e].pose.orientation = mul(qb, conj(half));
                std::cout << "replay native game rotation eye " << e << ": "
                          << 2.0 * std::acos(std::min(1.0, double(std::abs(r.w)))) * 57.29578 << " degrees\n";
            }
            native_a = rotated_a;
        }
        const std::span<const xrfg::D3D12ReprojectionView> native_target =
            game_rotation ? std::span<const xrfg::D3D12ReprojectionView>(rotated_target) : views1;
        if (!std::getenv("XRFG_TEST_REPLAY_NATIVE_B_CAMERA") && views1.size() == views2.size() &&
            !views1.empty()) {
            std::array<std::array<float, 4>, 2> rotations{};
            for (std::size_t view = 0; view < rotations.size(); ++view) {
                const std::size_t index = std::min(view, views1.size() - 1);
                rotations[view] = xrfg::synthetic_camera_to_current(
                    unaligned ? midpoint[index] : views2[index], native_target[index]);
            }
            outputs[0].camera_to_current = rotations;
        }
        xrfg::D3D12NativeDlssG native;
        require_hresult(native.initialize(fixture.device(), fixture.queue(), a->GetDesc(), f0.view_format),
                        "replay native initialization");
        // XRFG_TEST_REPLAY_NATIVE_CAMERA_FIT=1 hands NGX the camera's motion
        // between the frames in place of none: a rigid motion (rotation and
        // translation) fitted to B's vectors and depth over a 64x64 grid by
        // Gauss-Newton on the reprojection error, keeping the closest 70% of
        // the points (the scene, not what moves with the camera) after the
        // first passes. One eye per feature: run with XRFG_NATIVE_DLSSG_PER_EYE=1.
        // XRFG_TEST_REPLAY_NATIVE_OBJECT_MOTION=1 also takes the camera's own
        // flow out of the vectors, so NGX gets object motion alone and the
        // camera's from the matrices, as its contract has it. The fit applies
        // to the generating pair alone: the seeding pair has one frame on
        // both sides.
        struct CameraFit {
            DirectX::XMFLOAT4X4 clip{};
            std::optional<std::array<float, 7>> rigid;
        };
        std::array<std::optional<CameraFit>, 2> camera_fits{};
        const bool object_motion = std::getenv("XRFG_TEST_REPLAY_NATIVE_OBJECT_MOTION") != nullptr;
        if (std::getenv("XRFG_TEST_REPLAY_NATIVE_CAMERA_FIT")) {
            const auto half_to_float = [](std::uint16_t h) {
                const int exponent = (h >> 10) & 31, mantissa = h & 1023;
                float value = exponent == 0 ? std::ldexp(float(mantissa), -24)
                                            : std::ldexp(float(mantissa | 1024), exponent - 25);
                return (h & 0x8000) ? -value : value;
            };
            for (std::size_t e = 0; e < views2.size() && e < f2_guides.guides.size() && e < 2; ++e) {
                const auto dir = frame_dir(b_index + guide_shift);
                xrfg::DumpedTexture motion, depth;
                if (f2_guides.motion_files[e] == "-" || f2_guides.depth_files[e] == "-" ||
                    !xrfg::read_dumped_texture(dir / f2_guides.motion_files[e], &motion) ||
                    !xrfg::read_dumped_texture(dir / f2_guides.depth_files[e], &depth) ||
                    (motion.format != DXGI_FORMAT_R16G16_FLOAT && motion.format != DXGI_FORMAT_R32G32_FLOAT)) continue;
                const auto& g = f2_guides.guides[e];
                const auto& f = views2[e].fov;
                const double l = std::tan(f.angle_left), r = std::tan(f.angle_right);
                const double up = std::tan(f.angle_up), down = std::tan(f.angle_down);
                const auto mv = [&](UINT x, UINT y) {
                    const auto& s = motion.subresources[g.motion_slice];
                    const std::uint8_t* p = s.data.data() + std::size_t(y) * s.row_bytes;
                    if (motion.format == DXGI_FORMAT_R32G32_FLOAT) {
                        float v[2];
                        std::memcpy(v, p + std::size_t(x) * 8, 8);
                        return std::array<double, 2>{v[0], v[1]};
                    }
                    std::uint16_t h[2];
                    std::memcpy(h, p + std::size_t(x) * 4, 4);
                    return std::array<double, 2>{half_to_float(h[0]), half_to_float(h[1])};
                };
                const auto& ds = depth.subresources[0];
                const std::size_t depth_bytes = depth.width ? ds.row_bytes / depth.width : 4;
                const auto depth_at = [&](UINT x, UINT y) {
                    float d;
                    std::memcpy(&d, ds.data.data() + std::size_t(y) * ds.row_bytes + std::size_t(x) * depth_bytes, 4);
                    return double(d);
                };
                struct Point { std::array<double, 3> b; double ua, va; };
                std::vector<Point> points;
                constexpr UINT kGrid = 64;
                for (UINT j = 0; j < kGrid; ++j)
                    for (UINT i = 0; i < kGrid; ++i) {
                        const double u = (i + 0.5) / kGrid, v = (j + 0.5) / kGrid;
                        const auto m = mv(g.motion_x + UINT(u * g.motion_width), g.motion_y + UINT(v * g.motion_height));
                        const double d = depth_at(g.depth_x + UINT(u * g.depth_width), g.depth_y + UINT(v * g.depth_height));
                        const double du = m[0] * g.scale_x * vector_scale / g.motion_width;
                        const double dv = m[1] * g.scale_y * vector_scale / g.motion_height;
                        if (!std::isfinite(du) || !std::isfinite(dv) || !std::isfinite(d) || std::abs(du) > 1 || std::abs(dv) > 1) continue;
                        // Reversed infinite depth: distance is near / depth; the sky lies far off.
                        const double z = g.depth_inverted ? (d > 1e-7 ? g.camera_near / d : 1e7)
                                                          : (d < 1 - 1e-7 ? g.camera_near / (1 - d) : 1e7);
                        const double xt = l + u * (r - l), yt = up - v * (up - down);
                        points.push_back({{xt * z, yt * z, -z}, u + du, v + dv});
                    }
                if (points.size() < 64) continue;
                // x: axis-angle rotation then translation, taking B's camera space to A's.
                const auto rotation = [](const double* w) {
                    const double a = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                    std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
                    if (a < 1e-12) return m;
                    const double x = w[0] / a, y = w[1] / a, z = w[2] / a, c = std::cos(a), s = std::sin(a), t = 1 - c;
                    return std::array<double, 9>{t * x * x + c, t * x * y - s * z, t * x * z + s * y,
                                                 t * x * y + s * z, t * y * y + c, t * y * z - s * x,
                                                 t * x * z - s * y, t * y * z + s * x, t * z * z + c};
                };
                const auto residual = [&](const double* x, const Point& p, double* out) {
                    const auto R = rotation(x);
                    const double ax = R[0] * p.b[0] + R[1] * p.b[1] + R[2] * p.b[2] + x[3];
                    const double ay = R[3] * p.b[0] + R[4] * p.b[1] + R[5] * p.b[2] + x[4];
                    const double az = R[6] * p.b[0] + R[7] * p.b[1] + R[8] * p.b[2] + x[5];
                    const double w = -az > 1e-6 ? -az : 1e-6;
                    out[0] = ((ax / w - l) / (r - l) - p.ua) * g.motion_width;
                    out[1] = ((up - ay / w) / (up - down) - p.va) * g.motion_height;
                };
                double x[6]{};
                std::vector<std::uint8_t> use(points.size(), 1);
                for (int pass = 0; pass < 12; ++pass) {
                    double h[6][6]{}, b[6]{};
                    const double step[6]{1e-6, 1e-6, 1e-6, 1e-3, 1e-3, 1e-3};
                    std::vector<double> norms(points.size());
                    for (std::size_t k = 0; k < points.size(); ++k) {
                        double r0[2];
                        residual(x, points[k], r0);
                        norms[k] = std::hypot(r0[0], r0[1]);
                        if (!use[k]) continue;
                        double jac[2][6];
                        for (int c = 0; c < 6; ++c) {
                            double xs[6];
                            std::copy(x, x + 6, xs);
                            xs[c] += step[c];
                            double r1[2];
                            residual(xs, points[k], r1);
                            jac[0][c] = (r1[0] - r0[0]) / step[c];
                            jac[1][c] = (r1[1] - r0[1]) / step[c];
                        }
                        for (int a = 0; a < 6; ++a) {
                            b[a] -= jac[0][a] * r0[0] + jac[1][a] * r0[1];
                            for (int c = 0; c < 6; ++c) h[a][c] += jac[0][a] * jac[0][c] + jac[1][a] * jac[1][c];
                        }
                    }
                    // Solve h dx = b by Gaussian elimination with partial pivoting.
                    double dx[6]{};
                    bool solved = true;
                    for (int c = 0; c < 6 && solved; ++c) {
                        int pivot = c;
                        for (int k = c + 1; k < 6; ++k) if (std::abs(h[k][c]) > std::abs(h[pivot][c])) pivot = k;
                        if (std::abs(h[pivot][c]) < 1e-18) { solved = false; break; }
                        std::swap(h[c], h[pivot]); std::swap(b[c], b[pivot]);
                        for (int k = c + 1; k < 6; ++k) {
                            const double factor = h[k][c] / h[c][c];
                            for (int m = c; m < 6; ++m) h[k][m] -= factor * h[c][m];
                            b[k] -= factor * b[c];
                        }
                    }
                    if (!solved) break;
                    for (int c = 5; c >= 0; --c) {
                        double sum = b[c];
                        for (int m = c + 1; m < 6; ++m) sum -= h[c][m] * dx[m];
                        dx[c] = sum / h[c][c];
                    }
                    for (int c = 0; c < 6; ++c) x[c] += dx[c];
                    if (pass >= 2) {
                        std::vector<double> sorted(norms);
                        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() * 7 / 10, sorted.end());
                        const double cut = sorted[sorted.size() * 7 / 10];
                        for (std::size_t k = 0; k < points.size(); ++k) use[k] = norms[k] <= cut;
                    }
                }
                std::vector<double> inlier;
                for (std::size_t k = 0; k < points.size(); ++k) {
                    if (!use[k]) continue;
                    double rr[2];
                    residual(x, points[k], rr);
                    inlier.push_back(std::hypot(rr[0], rr[1]));
                }
                std::nth_element(inlier.begin(), inlier.begin() + inlier.size() / 2, inlier.end());
                const auto R = rotation(x);
                // Row vectors: B's view point times M is A's, M = [R^T 0; t 1].
                const DirectX::XMMATRIX motion_m(float(R[0]), float(R[3]), float(R[6]), 0.0F,
                                                 float(R[1]), float(R[4]), float(R[7]), 0.0F,
                                                 float(R[2]), float(R[5]), float(R[8]), 0.0F,
                                                 float(x[3]), float(x[4]), float(x[5]), 1.0F);
                const float n = g.camera_near, far_plane = g.camera_far;
                const float pz = g.depth_infinite ? (g.depth_inverted ? 0.f : -1.f)
                                 : g.depth_inverted ? n / (far_plane - n) : far_plane / (n - far_plane);
                const float pw = g.depth_infinite ? (g.depth_inverted ? n : -n)
                                 : g.depth_inverted ? n * far_plane / (far_plane - n) : n * far_plane / (n - far_plane);
                const DirectX::XMMATRIX projection(float(2 / (r - l)), 0, 0, 0, 0, float(2 / (up - down)), 0, 0,
                                                   float((r + l) / (r - l)), float((up + down) / (up - down)), pz, -1,
                                                   0, 0, pw, 0);
                const DirectX::XMMATRIX clip_to_previous =
                    DirectX::XMMatrixInverse(nullptr, projection) * motion_m * projection;
                DirectX::XMFLOAT4X4 stored;
                DirectX::XMStoreFloat4x4(&stored, clip_to_previous);
                CameraFit& fit = camera_fits[e].emplace();
                fit.clip = stored;
                const double angle_radians = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                if (object_motion) {
                    const double s = angle_radians > 1e-12 ? std::sin(angle_radians / 2) / angle_radians : 0.5;
                    fit.rigid = {{float(x[0] * s), float(x[1] * s), float(x[2] * s),
                                  float(std::cos(angle_radians / 2)), float(x[3]), float(x[4]), float(x[5])}};
                }
                const double angle = angle_radians * 57.29578;
                std::cout << "replay native camera fit eye " << e << ": " << angle << " degrees, translation "
                          << x[3] << ' ' << x[4] << ' ' << x[5] << ", median inlier residual "
                          << inlier[inlier.size() / 2] << " px over " << points.size() << " points"
                          << (object_motion ? ", object motion alone" : "") << '\n';
            }
        }
        auto seed_a = make_guides(f0, 1, 1.0F, nullptr);
        auto seed_b = make_guides(f0, 2, 1.0F, nullptr);
        auto moved = make_guides(f2_guides, 3, vector_scale, &f0);
        HRESULT seeded = E_FAIL, generated = E_FAIL;
        const auto fence_fits = [&] {
            // The work is done; a fence signalled now says so to the native
            // object, which reads the fit back before the next pair.
            ComPtr<ID3D12Fence> fence;
            require_hresult(fixture.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                            "replay fit fence");
            require_hresult(fixture.queue()->Signal(fence.Get(), 1), "replay fit signal");
            fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
            native.submitted(fence.Get(), 1);
        };
        if (gpu_fit && fit_previous) {
            // The pair before, frame -1 to frame 0, seeds and is fitted.
            auto previous = upload_dumped(fixture, f_previous->colour, colour_flags, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto previous_guides = make_guides(*f_previous, 1, 1.0F, nullptr);
            auto a_from_previous = make_guides(f0, 2, 1.0F, &*f_previous);
            const std::span<const xrfg::D3D12ReprojectionView> views_previous(f_previous->views);
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                seeded = native.record(list, 0, previous.Get(), a.Get(), views_previous, native_a,
                                       previous_guides.get(), a_from_previous.get(), outputs,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            });
            fence_fits();
        } else {
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                seeded = native.record(list, 0, a.Get(), a.Get(), native_a, native_a, seed_a.get(), seed_b.get(),
                                       outputs, D3D12_RESOURCE_STATE_RENDER_TARGET);
            });
        }
        for (std::size_t e = 0; e < camera_fits.size(); ++e) {
            if (camera_fits[e]) {
                native.set_test_camera_motion(UINT(e), &camera_fits[e]->clip.m[0][0],
                                              camera_fits[e]->rigid ? camera_fits[e]->rigid->data() : nullptr);
            }
        }
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            generated = native.record(list, 0, a.Get(), b.Get(), native_a, views2, seed_b.get(), moved.get(),
                                      outputs, D3D12_RESOURCE_STATE_RENDER_TARGET);
        });
        if (gpu_fit && generated == S_OK) {
            if (!fit_previous) {
                // The same pair again, with its own fit.
                fence_fits();
                fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                    generated = native.record(list, 0, a.Get(), b.Get(), native_a, views2, seed_b.get(), moved.get(),
                                              outputs, D3D12_RESOURCE_STATE_RENDER_TARGET);
                });
            }
            // The fit the generating pair was handed, then the pair's own:
            // the fit of its vectors as packed, less that camera flow, which
            // the pair after it would take as the camera's motion entire.
            // With the flow added back in the fit the two agree on a steady
            // turn; fitting the packed vectors as they were measured what
            // was left over instead.
            const auto print_fits = [&](const char* which) {
                for (UINT eye = 0; eye < 2; ++eye) {
                    if (const auto fit = native.last_camera_fit(eye)) {
                        std::cout << "replay native gpu fit " << which << " eye " << eye << ": " << fit->degrees
                                  << " degrees, translation " << fit->translation[0] << ' ' << fit->translation[1]
                                  << ' ' << fit->translation[2] << ", median residual " << fit->residual_px
                                  << " px, inliers " << fit->inliers << " of " << fit->points << '\n';
                    }
                }
            };
            print_fits("handed");
            fence_fits();
            native.poll_camera_fits();
            print_fits("own");
            std::cout << "replay native gpu fits used=" << native.camera_fits_used()
                      << " rejected=" << native.camera_fits_rejected() << '\n';
        }
        std::cout << "replay native " << scale << "% seeded=" << seeded << " generated=" << generated
                  << " skip=" << int(native.last_skip()) << '\n';
        if (generated == S_OK)
            score((std::string("native DLSS FG ") + scale + "%").c_str(),
                  read_back_texture(fixture, output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET));
    }
    SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_SCALE", nullptr);
    if (gpu_fit) SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_CAMERA", nullptr);
#endif
}
