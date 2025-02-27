/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Endian.h>
#include <AK/FixedArray.h>
#include <AK/LEB128.h>
#include <AK/MemoryStream.h>
#include <AK/Variant.h>
#include <LibCore/File.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/ImageFormats/TinyVGLoader.h>
#include <LibGfx/Line.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Point.h>

namespace Gfx {

using VarUInt = LEB128<u32>;

static constexpr Array<u8, 2> TVG_MAGIC { 0x72, 0x56 };

enum class ColorEncoding : u8 {
    RGBA8888 = 0,
    RGB565 = 1,
    RGBAF32 = 2,
    Custom = 3
};

enum class CoordinateRange : u8 {
    Default = 0,
    Reduced = 1,
    Enhanced = 2
};

enum class StyleType : u8 {
    FlatColored = 0,
    LinearGradient = 1,
    RadialGradinet = 2
};

enum class Command : u8 {
    EndOfDocument = 0,
    FillPolygon = 1,
    FillRectangles = 2,
    FillPath = 3,
    DrawLines = 4,
    DrawLineLoop = 5,
    DrawLineStrip = 6,
    DrawLinePath = 7,
    OutlineFillPolygon = 8,
    OutlineFillRectangles = 9,
    OutLineFillPath = 10
};

struct FillCommandHeader {
    u32 count;
    TinyVGDecodedImageData::Style style;
};

struct DrawCommandHeader {
    u32 count;
    TinyVGDecodedImageData::Style line_style;
    float line_width;
};

struct OutlineFillCommandHeader {
    u32 count;
    TinyVGDecodedImageData::Style fill_style;
    TinyVGDecodedImageData::Style line_style;
    float line_width;
};

enum class PathCommand : u8 {
    Line = 0,
    HorizontalLine = 1,
    VerticalLine = 2,
    CubicBezier = 3,
    ArcCircle = 4,
    ArcEllipse = 5,
    ClosePath = 6,
    QuadraticBezier = 7
};

struct TinyVGHeader {
    u8 version;
    u8 scale;
    ColorEncoding color_encoding;
    CoordinateRange coordinate_range;
    u32 width;
    u32 height;
    u32 color_count;
};

static ErrorOr<TinyVGHeader> decode_tinyvg_header(Stream& stream)
{
    TinyVGHeader header {};
    Array<u8, 2> magic_bytes;
    TRY(stream.read_until_filled(magic_bytes));
    if (magic_bytes != TVG_MAGIC)
        return Error::from_string_literal("Invalid TinyVG: Incorrect header magic");
    header.version = TRY(stream.read_value<u8>());
    u8 properties = TRY(stream.read_value<u8>());
    header.scale = properties & 0xF;
    header.color_encoding = static_cast<ColorEncoding>((properties >> 4) & 0x3);
    header.coordinate_range = static_cast<CoordinateRange>((properties >> 6) & 0x3);
    switch (header.coordinate_range) {
    case CoordinateRange::Default:
        header.width = TRY(stream.read_value<LittleEndian<u16>>());
        header.height = TRY(stream.read_value<LittleEndian<u16>>());
        break;
    case CoordinateRange::Reduced:
        header.width = TRY(stream.read_value<u8>());
        header.height = TRY(stream.read_value<u8>());
        break;
    case CoordinateRange::Enhanced:
        header.width = TRY(stream.read_value<LittleEndian<u32>>());
        header.height = TRY(stream.read_value<LittleEndian<u32>>());
        break;
    default:
        return Error::from_string_literal("Invalid TinyVG: Bad coordinate range");
    }
    header.color_count = TRY(stream.read_value<VarUInt>());
    return header;
}

static ErrorOr<FixedArray<Color>> decode_color_table(Stream& stream, ColorEncoding encoding, u32 color_count)
{
    if (encoding == ColorEncoding::Custom)
        return Error::from_string_literal("Invalid TinyVG: Unsupported color encoding");
    auto color_table = TRY(FixedArray<Color>::create(color_count));
    auto parse_color = [&]() -> ErrorOr<Color> {
        switch (encoding) {
        case ColorEncoding::RGBA8888: {
            Array<u8, 4> rgba;
            TRY(stream.read_until_filled(rgba));
            return Color(rgba[0], rgba[1], rgba[2], rgba[3]);
        }
        case ColorEncoding::RGB565: {
            u16 color = TRY(stream.read_value<LittleEndian<u16>>());
            auto red = (color >> (6 + 5)) & 0x1f;
            auto green = (color >> 5) & 0x3f;
            auto blue = (color >> 0) & 0x1f;
            return Color((red * 255 + 15) / 31, (green * 255 + 31), (blue * 255 + 15) / 31);
        }
        case ColorEncoding::RGBAF32: {
            auto red = TRY(stream.read_value<LittleEndian<f32>>());
            auto green = TRY(stream.read_value<LittleEndian<f32>>());
            auto blue = TRY(stream.read_value<LittleEndian<f32>>());
            auto alpha = TRY(stream.read_value<LittleEndian<f32>>());
            return Color(red * 255, green * 255, blue * 255, alpha * 255);
        }
        default:
            return Error::from_string_literal("Invalid TinyVG: Bad color encoding");
        }
    };
    for (auto& color : color_table) {
        color = TRY(parse_color());
    }
    return color_table;
}

class TinyVGReader {
public:
    TinyVGReader(Stream& stream, TinyVGHeader const& header, ReadonlySpan<Color> color_table)
        : m_stream(stream)
        , m_scale(powf(0.5, header.scale))
        , m_coordinate_range(header.coordinate_range)
        , m_color_table(color_table)
    {
    }

    ErrorOr<float> read_unit()
    {
        auto read_value = [&]() -> ErrorOr<i32> {
            switch (m_coordinate_range) {
            case CoordinateRange::Default:
                return TRY(m_stream.read_value<LittleEndian<i16>>());
            case CoordinateRange::Reduced:
                return TRY(m_stream.read_value<i8>());
            case CoordinateRange::Enhanced:
                return TRY(m_stream.read_value<LittleEndian<i32>>());
            default:
                // Note: Already checked while reading the header.
                VERIFY_NOT_REACHED();
            }
        };
        return TRY(read_value()) * m_scale;
    }

    ErrorOr<u32> read_var_uint()
    {
        return TRY(m_stream.read_value<VarUInt>());
    }

    ErrorOr<FloatPoint> read_point()
    {
        return FloatPoint { TRY(read_unit()), TRY(read_unit()) };
    }

    ErrorOr<TinyVGDecodedImageData::Style> read_style(StyleType type)
    {
        auto read_color = [&]() -> ErrorOr<Color> {
            auto color_index = TRY(m_stream.read_value<VarUInt>());
            return m_color_table[color_index];
        };
        switch (type) {
        case StyleType::FlatColored: {
            return TRY(read_color());
        }
        case StyleType::LinearGradient:
        case StyleType::RadialGradinet: {
            // TODO: Make PaintStyle (for these ultra basic gradients)
            [[maybe_unused]] auto point_0 = TRY(read_point());
            [[maybe_unused]] auto point_1 = TRY(read_point());
            [[maybe_unused]] auto color_0 = TRY(read_color());
            [[maybe_unused]] auto color_1 = TRY(read_color());
            return Color(Color::Black);
        }
        }
        return Error::from_string_literal("Invalid TinyVG: Bad style data");
    }

    ErrorOr<FloatRect> read_rectangle()
    {
        return FloatRect { TRY(read_unit()), TRY(read_unit()), TRY(read_unit()), TRY(read_unit()) };
    }

    ErrorOr<FloatLine> read_line()
    {
        return FloatLine { TRY(read_point()), TRY(read_point()) };
    }

    ErrorOr<Path> read_path(u32 segment_count)
    {
        Path path;
        auto segment_lengths = TRY(FixedArray<u32>::create(segment_count));
        for (auto& command_count : segment_lengths) {
            command_count = TRY(read_var_uint()) + 1;
        }
        for (auto command_count : segment_lengths) {
            auto start_point = TRY(read_point());
            path.move_to(start_point);
            for (u32 i = 0; i < command_count; i++) {
                u8 command_tag = TRY(m_stream.read_value<u8>());
                auto path_command = static_cast<PathCommand>(command_tag & 0x7);
                switch (path_command) {
                case PathCommand::Line:
                    path.line_to(TRY(read_point()));
                    break;
                case PathCommand::HorizontalLine:
                    path.line_to({ TRY(read_unit()), path.segments().last()->point().y() });
                    break;
                case PathCommand::VerticalLine:
                    path.line_to({ path.segments().last()->point().x(), TRY(read_unit()) });
                    break;
                case PathCommand::CubicBezier: {
                    auto control_0 = TRY(read_point());
                    auto control_1 = TRY(read_point());
                    auto point_1 = TRY(read_point());
                    path.cubic_bezier_curve_to(control_0, control_1, point_1);
                    break;
                }
                case PathCommand::ArcCircle: {
                    u8 flags = TRY(m_stream.read_value<u8>());
                    bool large_arc = (flags >> 0) & 0b1;
                    bool sweep = (flags >> 1) & 0b1;
                    auto radius = TRY(read_unit());
                    auto target = TRY(read_point());
                    path.arc_to(target, radius, large_arc, !sweep);
                    break;
                }
                case PathCommand::ArcEllipse: {
                    u8 flags = TRY(m_stream.read_value<u8>());
                    bool large_arc = (flags >> 0) & 0b1;
                    bool sweep = (flags >> 1) & 0b1;
                    auto radius_x = TRY(read_unit());
                    auto radius_y = TRY(read_unit());
                    auto rotation = TRY(read_unit());
                    auto target = TRY(read_point());
                    path.elliptical_arc_to(target, { radius_x, radius_y }, rotation, large_arc, !sweep);
                    break;
                }
                case PathCommand::ClosePath: {
                    path.close();
                    break;
                }
                case PathCommand::QuadraticBezier: {
                    auto control = TRY(read_point());
                    auto point_1 = TRY(read_point());
                    path.quadratic_bezier_curve_to(control, point_1);
                    break;
                }
                default:
                    return Error::from_string_literal("Invalid TinyVG: Bad path command");
                }
            }
        }
        return path;
    }

    ErrorOr<FillCommandHeader> read_fill_command_header(StyleType style_type)
    {
        return FillCommandHeader { TRY(read_var_uint()) + 1, TRY(read_style(style_type)) };
    }

    ErrorOr<DrawCommandHeader> read_draw_command_header(StyleType style_type)
    {
        return DrawCommandHeader { TRY(read_var_uint()) + 1, TRY(read_style(style_type)), TRY(read_unit()) };
    }

    ErrorOr<OutlineFillCommandHeader> read_outline_fill_command_header(StyleType style_type)
    {
        u8 header = TRY(m_stream.read_value<u8>());
        u8 count = (header & 0x3f) + 1;
        auto stroke_type = static_cast<StyleType>((header >> 6) & 0x3);
        return OutlineFillCommandHeader { count, TRY(read_style(style_type)), TRY(read_style(stroke_type)), TRY(read_unit()) };
    }

private:
    Stream& m_stream;
    float m_scale {};
    CoordinateRange m_coordinate_range;
    ReadonlySpan<Color> m_color_table;
};

ErrorOr<TinyVGDecodedImageData> TinyVGDecodedImageData::decode(Stream& stream)
{
    auto header = TRY(decode_tinyvg_header(stream));
    if (header.version != 1)
        return Error::from_string_literal("Invalid TinyVG: Unsupported version");

    auto color_table = TRY(decode_color_table(stream, header.color_encoding, header.color_count));
    TinyVGReader reader { stream, header, color_table.span() };

    auto rectangle_to_path = [](FloatRect const& rect) -> Path {
        Path path;
        path.move_to({ rect.x(), rect.y() });
        path.line_to({ rect.x() + rect.width(), rect.y() });
        path.line_to({ rect.x() + rect.width(), rect.y() + rect.height() });
        path.line_to({ rect.x(), rect.y() + rect.height() });
        path.close();
        return path;
    };

    Vector<DrawCommand> draw_commands;
    bool at_end = false;
    while (!at_end) {
        u8 command_info = TRY(stream.read_value<u8>());
        auto command = static_cast<Command>(command_info & 0x3f);
        auto style_type = static_cast<StyleType>((command_info >> 6) & 0x3);

        switch (command) {
        case Command::EndOfDocument:
            at_end = true;
            break;
        case Command::FillPolygon: {
            auto header = TRY(reader.read_fill_command_header(style_type));
            Path polygon;
            polygon.move_to(TRY(reader.read_point()));
            for (u32 i = 0; i < header.count - 1; i++)
                polygon.line_to(TRY(reader.read_point()));
            TRY(draw_commands.try_append(DrawCommand { move(polygon), move(header.style) }));
            break;
        }
        case Command::FillRectangles: {
            auto header = TRY(reader.read_fill_command_header(style_type));
            for (u32 i = 0; i < header.count; i++) {
                TRY(draw_commands.try_append(DrawCommand {
                    rectangle_to_path(TRY(reader.read_rectangle())), move(header.style) }));
            }
            break;
        }
        case Command::FillPath: {
            auto header = TRY(reader.read_fill_command_header(style_type));
            auto path = TRY(reader.read_path(header.count));
            TRY(draw_commands.try_append(DrawCommand { move(path), move(header.style) }));
            break;
        }
        case Command::DrawLines: {
            auto header = TRY(reader.read_draw_command_header(style_type));
            Path path;
            for (u32 i = 0; i < header.count; i++) {
                auto line = TRY(reader.read_line());
                path.move_to(line.a());
                path.line_to(line.b());
            }
            TRY(draw_commands.try_append(DrawCommand { move(path), {}, move(header.line_style), header.line_width }));
            break;
        }
        case Command::DrawLineStrip:
        case Command::DrawLineLoop: {
            auto header = TRY(reader.read_draw_command_header(style_type));
            Path path;
            path.move_to(TRY(reader.read_point()));
            for (u32 i = 0; i < header.count - 1; i++)
                path.line_to(TRY(reader.read_point()));
            if (command == Command::DrawLineLoop)
                path.close();
            TRY(draw_commands.try_append(DrawCommand { move(path), {}, move(header.line_style), header.line_width }));
            break;
        }
        case Command::DrawLinePath: {
            auto header = TRY(reader.read_draw_command_header(style_type));
            auto path = TRY(reader.read_path(header.count));
            TRY(draw_commands.try_append(DrawCommand { move(path), {}, move(header.line_style), header.line_width }));
            break;
        }
        case Command::OutlineFillPolygon: {
            auto header = TRY(reader.read_outline_fill_command_header(style_type));
            Path polygon;
            polygon.move_to(TRY(reader.read_point()));
            for (u32 i = 0; i < header.count - 1; i++)
                polygon.line_to(TRY(reader.read_point()));
            TRY(draw_commands.try_append(DrawCommand { move(polygon), move(header.fill_style), move(header.line_style), header.line_width }));
            break;
        }
        case Command::OutlineFillRectangles: {
            auto header = TRY(reader.read_outline_fill_command_header(style_type));
            for (u32 i = 0; i < header.count - 1; i++) {
                TRY(draw_commands.try_append(DrawCommand {
                    rectangle_to_path(TRY(reader.read_rectangle())), move(header.fill_style), move(header.line_style), header.line_width }));
            }
            break;
        }
        case Command::OutLineFillPath: {
            auto header = TRY(reader.read_outline_fill_command_header(style_type));
            auto path = TRY(reader.read_path(header.count));
            TRY(draw_commands.try_append(DrawCommand { move(path), move(header.fill_style), move(header.line_style), header.line_width }));
            break;
        }
        default:
            return Error::from_string_literal("Invalid TinyVG: Bad command");
        }
    }

    return TinyVGDecodedImageData { { header.width, header.height }, move(draw_commands) };
}

ErrorOr<RefPtr<Gfx::Bitmap>> TinyVGDecodedImageData::bitmap(IntSize size) const
{
    auto scale_x = float(size.width()) / m_size.width();
    auto scale_y = float(size.height()) / m_size.height();
    auto transform = Gfx::AffineTransform {}.scale(scale_x, scale_y);
    auto bitmap = TRY(Bitmap::create(Gfx::BitmapFormat::BGRA8888, size));
    Painter base_painter { *bitmap };
    AntiAliasingPainter painter { base_painter };
    for (auto const& command : draw_commands()) {
        auto draw_path = command.path.copy_transformed(transform);
        if (command.fill.has_value()) {
            auto fill_path = draw_path;
            fill_path.close_all_subpaths();
            command.fill->visit(
                [&](Color color) { painter.fill_path(fill_path, color, Painter::WindingRule::EvenOdd); },
                [&](NonnullRefPtr<SVGGradientPaintStyle> style) {
                    const_cast<SVGGradientPaintStyle&>(*style).set_gradient_transform(transform);
                    painter.fill_path(fill_path, style, 1.0f, Painter::WindingRule::EvenOdd);
                });
        }
        if (command.stroke.has_value()) {
            // FIXME: A more correct way to non-uniformly scale strokes would be:
            //  1. Scale the path uniformly by the largest of scale_x/y
            //  2. Convert that to a fill with .stroke_to_fill()
            //  3.
            //     If scale_x > scale_y
            //        Scale by: (1, scale_y/scale_x)
            //     else
            //        Scale by: (scale_x/scale_y, 1)
            auto stroke_scale = max(scale_x, scale_y);
            command.stroke->visit(
                [&](Color color) { painter.stroke_path(draw_path, color, command.stroke_width * stroke_scale); },
                [&](NonnullRefPtr<SVGGradientPaintStyle> style) {
                    const_cast<SVGGradientPaintStyle&>(*style).set_gradient_transform(transform);
                    painter.stroke_path(draw_path, style, command.stroke_width * stroke_scale);
                });
        }
    }
    return bitmap;
}

TinyVGImageDecoderPlugin::TinyVGImageDecoderPlugin(ReadonlyBytes bytes)
    : m_context { bytes }
{
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> TinyVGImageDecoderPlugin::create(ReadonlyBytes bytes)
{
    return adopt_nonnull_own_or_enomem(new (nothrow) TinyVGImageDecoderPlugin(bytes));
}

bool TinyVGImageDecoderPlugin::sniff(ReadonlyBytes bytes)
{
    FixedMemoryStream stream { { bytes.data(), bytes.size() } };
    return !decode_tinyvg_header(stream).is_error();
}

IntSize TinyVGImageDecoderPlugin::size()
{
    if (m_context.decoded_image)
        return m_context.decoded_image->size();
    return {};
}

ErrorOr<void> TinyVGImageDecoderPlugin::initialize()
{
    FixedMemoryStream stream { { m_context.data.data(), m_context.data.size() } };
    m_context.decoded_image = make<TinyVGDecodedImageData>(TRY(TinyVGDecodedImageData::decode(stream)));
    return {};
}

ErrorOr<ImageFrameDescriptor> TinyVGImageDecoderPlugin::frame(size_t, Optional<IntSize> ideal_size)
{
    auto target_size = ideal_size.value_or(m_context.decoded_image->size());
    if (!m_context.bitmap || m_context.bitmap->size() != target_size)
        m_context.bitmap = TRY(m_context.decoded_image->bitmap(target_size));
    return ImageFrameDescriptor { m_context.bitmap };
}

}
