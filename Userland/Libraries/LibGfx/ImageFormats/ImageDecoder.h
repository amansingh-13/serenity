/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Size.h>

namespace Gfx {

class Bitmap;

static constexpr size_t maximum_width_for_decoded_images = 16384;
static constexpr size_t maximum_height_for_decoded_images = 16384;

struct ImageFrameDescriptor {
    RefPtr<Bitmap> image;
    int duration { 0 };
};

class ImageDecoderPlugin {
public:
    virtual ~ImageDecoderPlugin() = default;

    virtual IntSize size() = 0;

    virtual ErrorOr<void> initialize() { return {}; }

    virtual bool is_animated() = 0;
    virtual size_t loop_count() = 0;
    virtual size_t frame_count() = 0;
    virtual size_t first_animated_frame_index() = 0;
    virtual ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) = 0;
    virtual ErrorOr<Optional<ReadonlyBytes>> icc_data() = 0;

protected:
    ImageDecoderPlugin() = default;
};

class ImageDecoder : public RefCounted<ImageDecoder> {
public:
    static RefPtr<ImageDecoder> try_create_for_raw_bytes(ReadonlyBytes, Optional<DeprecatedString> mime_type = {});
    ~ImageDecoder() = default;

    IntSize size() const { return m_plugin->size(); }
    int width() const { return size().width(); }
    int height() const { return size().height(); }
    bool is_animated() const { return m_plugin->is_animated(); }
    size_t loop_count() const { return m_plugin->loop_count(); }
    size_t frame_count() const { return m_plugin->frame_count(); }
    size_t first_animated_frame_index() const { return m_plugin->first_animated_frame_index(); }
    ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) const { return m_plugin->frame(index, ideal_size); }
    ErrorOr<Optional<ReadonlyBytes>> icc_data() const { return m_plugin->icc_data(); }

private:
    explicit ImageDecoder(NonnullOwnPtr<ImageDecoderPlugin>);

    NonnullOwnPtr<ImageDecoderPlugin> mutable m_plugin;
};

}
