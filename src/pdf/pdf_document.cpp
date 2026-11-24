#include "nisaba/pdf/pdf_document.hpp"
#include "nisaba/pdf/pdf_writer.hpp"
#include "nisaba/markdown/renderer.hpp"
#include <fstream>
#include <algorithm>

namespace nisaba::pdf {

namespace {
inline Rect make_rect(float x, float y, float w, float h) {
    return Rect::from_xywh(x, y, w, h).value_or(Rect());
}
} // namespace

PdfPage::PdfPage(float width, float height)
    : width_(width), height_(height), canvas_(width, height) {}

PdfDocument::PdfDocument() = default;

PdfPage* PdfDocument::add_page(float width, float height) {
    pages_.push_back(std::make_unique<PdfPage>(width, height));
    return pages_.back().get();
}

PdfPage* PdfDocument::add_page(PageSize size, PageOrientation orientation) {
    float w = 595.28f;
    float h = 841.89f;

    switch (size) {
        case PageSize::A4:
            w = 595.28f; h = 841.89f; break;
        case PageSize::Letter:
            w = 612.0f; h = 792.0f; break;
        case PageSize::A3:
            w = 841.89f; h = 1190.55f; break;
        case PageSize::Legal:
            w = 612.0f; h = 1008.0f; break;
        case PageSize::Custom:
            break;
    }

    if (orientation == PageOrientation::Landscape) {
        std::swap(w, h);
    }

    return add_page(w, h);
}

PdfPage* PdfDocument::page(size_t index) {
    if (index >= pages_.size()) return nullptr;
    return pages_[index].get();
}

const PdfPage* PdfDocument::page(size_t index) const {
    if (index >= pages_.size()) return nullptr;
    return pages_[index].get();
}

std::vector<uint8_t> PdfDocument::save_to_bytes() {
    PdfWriter writer;

    uint32_t catalog_id = writer.allocate_id();
    uint32_t pages_root_id = writer.allocate_id();
    uint32_t info_id = writer.allocate_id();

    PdfRef pages_root_ref(pages_root_id, 0);
    PdfArray kids_array;
    kids_array.reserve(pages_.size());

    for (const auto& p : pages_) {
        uint32_t page_id = writer.allocate_id();
        kids_array.emplace_back(PdfRef(page_id, 0));

        // 1. Content Stream
        uint32_t content_id = writer.allocate_id();
        const std::string& strm_str = p->canvas().content_stream();
        std::span<const uint8_t> strm_span(
            reinterpret_cast<const uint8_t*>(strm_str.data()),
            strm_str.size()
        );
        writer.add_stream_object(content_id, PdfDict{}, strm_span, true);

        // 2. Resource dictionaries
        PdfDict font_dict;
        for (const auto& f : p->canvas().font_resources()) {
            uint32_t font_obj_id = writer.allocate_id();
            PdfDict font_def;
            font_def["Type"] = PdfName("Font");
            font_def["Subtype"] = PdfName("Type1");
            font_def["BaseFont"] = PdfName(f.base_font);
            writer.add_object(font_obj_id, std::move(font_def));
            font_dict[f.resource_name] = PdfRef(font_obj_id, 0);
        }

        PdfDict xobject_dict;
        for (const auto& img : p->canvas().image_resources()) {
            uint32_t w = img.width;
            uint32_t h = img.height;
            uint32_t img_obj_id = writer.allocate_id();

            // Extract RGB and Alpha
            std::vector<uint8_t> rgb_data;
            std::vector<uint8_t> alpha_data;
            rgb_data.reserve(w * h * 3);
            alpha_data.reserve(w * h);

            const auto* pixels = reinterpret_cast<const PremultipliedColorU8*>(img.data.data());
            bool has_alpha = false;
            size_t total_px = static_cast<size_t>(w) * static_cast<size_t>(h);
            for (size_t idx = 0; idx < total_px; ++idx) {
                PremultipliedColorU8 c = pixels[idx];
                rgb_data.push_back(c.red());
                rgb_data.push_back(c.green());
                rgb_data.push_back(c.blue());
                uint8_t a = c.alpha();
                alpha_data.push_back(a);
                if (a < 255) has_alpha = true;
            }

            PdfDict img_dict;
            img_dict["Type"] = PdfName("XObject");
            img_dict["Subtype"] = PdfName("Image");
            img_dict["Width"] = static_cast<int64_t>(w);
            img_dict["Height"] = static_cast<int64_t>(h);
            img_dict["ColorSpace"] = PdfName("DeviceRGB");
            img_dict["BitsPerComponent"] = 8;

            if (has_alpha) {
                uint32_t smask_id = writer.allocate_id();
                PdfDict smask_dict;
                smask_dict["Type"] = PdfName("XObject");
                smask_dict["Subtype"] = PdfName("Image");
                smask_dict["Width"] = static_cast<int64_t>(w);
                smask_dict["Height"] = static_cast<int64_t>(h);
                smask_dict["ColorSpace"] = PdfName("DeviceGray");
                smask_dict["BitsPerComponent"] = 8;
                writer.add_stream_object(smask_id, std::move(smask_dict), alpha_data, true);
                img_dict["SMask"] = PdfRef(smask_id, 0);
            }

            writer.add_stream_object(img_obj_id, std::move(img_dict), rgb_data, true);
            xobject_dict[img.name] = PdfRef(img_obj_id, 0);
        }

        PdfDict extgstate_dict;
        for (const auto& [key, name] : p->canvas().extgstate_resources()) {
            uint32_t gs_id = writer.allocate_id();
            double alpha = key / 1000.0;
            PdfDict gs_def;
            gs_def["Type"] = PdfName("ExtGState");
            gs_def["ca"] = alpha;
            gs_def["CA"] = alpha;
            writer.add_object(gs_id, std::move(gs_def));
            extgstate_dict[name] = PdfRef(gs_id, 0);
        }

        PdfDict res_dict;
        if (!font_dict.empty()) res_dict["Font"] = std::move(font_dict);
        if (!xobject_dict.empty()) res_dict["XObject"] = std::move(xobject_dict);
        if (!extgstate_dict.empty()) res_dict["ExtGState"] = std::move(extgstate_dict);

        // 3. Page Object
        PdfDict page_obj;
        page_obj["Type"] = PdfName("Page");
        page_obj["Parent"] = pages_root_ref;
        page_obj["MediaBox"] = PdfArray{0.0, 0.0, p->width(), p->height()};
        page_obj["Contents"] = PdfRef(content_id, 0);
        page_obj["Resources"] = std::move(res_dict);

        // 3b. ISO 32000-1 Page Annotations (/Annots)
        const auto& page_annots = p->canvas().annotations();
        if (!page_annots.empty()) {
            PdfArray annots_array;
            annots_array.reserve(page_annots.size());
            const double page_h = static_cast<double>(p->height());

            for (const auto& annot : page_annots) {
                uint32_t annot_id = writer.allocate_id();
                PdfDict annot_dict;
                annot_dict["Type"] = PdfName("Annot");

                // Convert top-down Nisaba coordinates to bottom-up PDF page coordinates.
                const double x0 = static_cast<double>(annot.rect.left());
                const double x1 = static_cast<double>(annot.rect.right());
                const double pdf_y0 = page_h - static_cast<double>(annot.rect.bottom());
                const double pdf_y1 = page_h - static_cast<double>(annot.rect.top());
                annot_dict["Rect"] = PdfArray{x0, pdf_y0, x1, pdf_y1};

                std::string value_str;
                if (!annot.data.empty()) {
                    size_t len = annot.data.size();
                    while (len > 0 && annot.data[len - 1] == 0) {
                        --len;
                    }
                    value_str.assign(reinterpret_cast<const char*>(annot.data.data()), len);
                }

                if (annot.key == "URL" || annot.key == "URI" || annot.key == "Link" || annot.key == "Rect_With_URL") {
                    annot_dict["Subtype"] = PdfName("Link");
                    annot_dict["Border"] = PdfArray{0, 0, 0};
                    PdfDict action;
                    action["Type"] = PdfName("Action");
                    action["S"] = PdfName("URI");
                    action["URI"] = value_str;
                    annot_dict["A"] = std::move(action);
                } else if (annot.key == "NamedDest" || annot.key == "LinkToDest" ||
                           annot.key == "Link_To_Name" || annot.key == "Define_Named_Dest") {
                    annot_dict["Subtype"] = PdfName("Link");
                    annot_dict["Border"] = PdfArray{0, 0, 0};
                    annot_dict["Dest"] = value_str;
                } else {
                    annot_dict["Subtype"] = PdfName("Text");
                    annot_dict["T"] = annot.key;
                    annot_dict["Contents"] = value_str;
                    annot_dict["F"] = static_cast<int64_t>(2); // Hidden metadata flag
                }

                writer.add_object(annot_id, std::move(annot_dict));
                annots_array.push_back(PdfRef(annot_id, 0));
            }
            page_obj["Annots"] = std::move(annots_array);
        }

        writer.add_object(page_id, std::move(page_obj));
    }

    // 4. Pages Root Object
    PdfDict pages_root;
    pages_root["Type"] = PdfName("Pages");
    pages_root["Kids"] = std::move(kids_array);
    pages_root["Count"] = static_cast<int64_t>(pages_.size());
    writer.add_object(pages_root_id, std::move(pages_root));

    // 5. Catalog Object
    PdfDict catalog;
    catalog["Type"] = PdfName("Catalog");
    catalog["Pages"] = pages_root_ref;
    writer.add_object(catalog_id, std::move(catalog));
    writer.set_root(PdfRef(catalog_id, 0));

    // 6. Info Object
    PdfDict info;
    info["Title"] = title_;
    info["Author"] = author_;
    info["Subject"] = subject_;
    info["Creator"] = creator_;
    info["Producer"] = "Nisaba 2D Sovereign Engine";
    writer.add_object(info_id, std::move(info));
    writer.set_info(PdfRef(info_id, 0));

    return writer.to_bytes();
}

bool PdfDocument::save_to_file(const std::string& path) {
    auto bytes = save_to_bytes();
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return out.good();
}

bool PdfDocument::export_markdown(
    markdown::MarkdownDocument& doc,
    const markdown::MarkdownStyle& style,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache,
    float page_width,
    float page_height,
    float margin
) {
    if (!doc.is_valid()) return false;

    float content_width = page_width - (margin * 2.0f);
    float content_height = page_height - (margin * 2.0f);
    if (content_width <= 20.0f || content_height <= 20.0f) return false;

    // Run layout pass
    doc.layout(content_width, style, font_system, glyph_cache);

    PdfPage* cur_page = add_page(page_width, page_height);
    float cur_y = margin;

    for (const auto& block_ptr : doc.root()->children) {
        if (!block_ptr) continue;
        const auto& block = *block_ptr;

        // If block exceeds remaining page height, start a new page
        if (cur_y + block.layout_height > margin + content_height && cur_y > margin) {
            cur_page = add_page(page_width, page_height);
            cur_y = margin;
        }

        // Render block onto current page canvas
        auto& cvs = cur_page->canvas();

        switch (block.type) {
            case markdown::BlockType::Heading: {
                int idx = std::clamp(block.heading_level - 1, 0, 5);
                float font_sz = style.heading_sizes[idx];
                std::string font_name = "Helvetica-Bold";

                std::string text_str;
                for (const auto& span : block.inlines) {
                    text_str += span.text;
                }
                cvs.draw_text(text_str, margin, cur_y, font_sz, style.heading_color, font_name);

                // Optional heading underline for H1/H2
                if (block.heading_level <= 2) {
                    float line_y = cur_y + font_sz * 1.25f;
                    cvs.stroke_rect(
                        make_rect(margin, line_y, content_width, 1.0f),
                        Paint(style.divider_color),
                        Stroke(1.0f)
                    );
                }
                break;
            }
            case markdown::BlockType::Paragraph: {
                std::string text_str;
                for (const auto& span : block.inlines) {
                    text_str += span.text;
                }
                cvs.draw_text(text_str, margin, cur_y, style.base_font_size, style.text_color, "Helvetica");
                break;
            }
            case markdown::BlockType::CodeBlock: {
                Rect card_rect = make_rect(margin, cur_y, content_width, block.layout_height);
                cvs.fill_rect(card_rect, Paint(style.code_block_bg));
                cvs.stroke_rect(card_rect, Paint(style.code_block_border), Stroke(1.0f));

                std::string code_text;
                for (size_t l = 0; l < block.code_lines.size(); ++l) {
                    if (l > 0) code_text += '\n';
                    code_text += block.code_lines[l];
                }

                cvs.draw_text(
                    code_text,
                    margin + style.code_block_padding,
                    cur_y + style.code_block_padding,
                    style.code_font_size,
                    style.code_text_color,
                    "Courier"
                );
                break;
            }
            case markdown::BlockType::BlockQuote: {
                cvs.fill_rect(
                    make_rect(margin, cur_y, 4.0f, block.layout_height),
                    Paint(style.quote_bar_color)
                );
                std::string text_str;
                for (const auto& span : block.inlines) {
                    text_str += span.text;
                }
                cvs.draw_text(
                    text_str,
                    margin + 12.0f,
                    cur_y + 2.0f,
                    style.quote_font_size,
                    style.quote_text_color,
                    "Helvetica-Oblique"
                );
                break;
            }
            case markdown::BlockType::ThematicBreak: {
                float line_y = cur_y + block.layout_height * 0.5f;
                cvs.stroke_rect(
                    make_rect(margin, line_y, content_width, 1.0f),
                    Paint(style.divider_color),
                    Stroke(1.0f)
                );
                break;
            }
            case markdown::BlockType::ListItem: {
                std::string marker = "• ";
                if (block.is_ordered_list) {
                    marker = std::to_string(block.list_start_number) + ". ";
                }
                cvs.draw_text(marker, margin, cur_y, style.base_font_size, style.text_color, "Helvetica-Bold");

                std::string text_str;
                for (const auto& span : block.inlines) {
                    text_str += span.text;
                }
                cvs.draw_text(text_str, margin + 20.0f, cur_y, style.base_font_size, style.text_color, "Helvetica");
                break;
            }
            case markdown::BlockType::Table: {
                // Table background & border
                Rect table_rect = make_rect(margin, cur_y, content_width, block.layout_height);
                cvs.stroke_rect(table_rect, Paint(style.table_border_color), Stroke(1.0f));

                if (!block.table_rows.empty()) {
                    float row_h = block.layout_height / static_cast<float>(block.table_rows.size());
                    float col_w = content_width / static_cast<float>(std::max<size_t>(1, block.table_rows[0].cells.size()));

                    for (size_t r = 0; r < block.table_rows.size(); ++r) {
                        float row_y = cur_y + static_cast<float>(r) * row_h;
                        if (r == 0) {
                            cvs.fill_rect(make_rect(margin, row_y, content_width, row_h), Paint(style.table_header_bg));
                        } else if (r % 2 == 1) {
                            cvs.fill_rect(make_rect(margin, row_y, content_width, row_h), Paint(style.table_row_alt_bg));
                        }

                        // Cell texts
                        for (size_t c = 0; c < block.table_rows[r].cells.size(); ++c) {
                            float cell_x = margin + static_cast<float>(c) * col_w + 6.0f;
                            std::string cell_text;
                            for (const auto& span : block.table_rows[r].cells[c].inlines) {
                                cell_text += span.text;
                            }
                            std::string_view font_name = (r == 0) ? "Helvetica-Bold" : "Helvetica";
                            cvs.draw_text(cell_text, cell_x, row_y + 4.0f, style.base_font_size * 0.9f, style.text_color, font_name);
                        }

                        // Row divider
                        if (r > 0) {
                            cvs.stroke_rect(make_rect(margin, row_y, content_width, 1.0f), Paint(style.table_border_color), Stroke(0.5f));
                        }
                    }
                }
                break;
            }
            default:
                break;
        }

        cur_y += block.layout_height + style.paragraph_spacing;
    }

    return true;
}

} // namespace nisaba::pdf
