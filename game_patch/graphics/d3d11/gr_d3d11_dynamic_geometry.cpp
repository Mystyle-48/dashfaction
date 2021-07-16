#include <cassert>
#include "gr_d3d11.h"
#include "gr_d3d11_dynamic_geometry.h"
#include "gr_d3d11_shader.h"
#include "gr_d3d11_context.h"
#include "../../rf/os/frametime.h"
#define NO_D3D8
#include "../../rf/gr/gr_direct3d.h"

using namespace rf;

namespace df::gr::d3d11
{
    constexpr int batch_max_vertex = 6000;
    constexpr int batch_max_index = 10000;

    DynamicGeometryRenderer::DynamicGeometryRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, RenderContext& render_context) :
        device_{device}, render_context_(render_context),
        vertex_ring_buffer_{batch_max_vertex, D3D11_BIND_VERTEX_BUFFER, device_, render_context.device_context()},
        index_ring_buffer_{batch_max_index, D3D11_BIND_INDEX_BUFFER, device_, render_context.device_context()}
    {
        vertex_shader_ = shader_manager.get_vertex_shader(VertexShaderId::transformed);
        pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::standard);
    }

    void DynamicGeometryRenderer::flush()
    {
        auto [start_vertex, num_vertex] = vertex_ring_buffer_.submit();
        if (num_vertex == 0) {
            return;
        }
        auto [start_index, num_index] = index_ring_buffer_.submit();

        xlog::trace("Drawing dynamic geometry num_vertex %d num_index %d texture %s",
            num_vertex, num_index, rf::bm::get_filename(textures_[0]));

        render_context_.set_vertex_buffer(vertex_ring_buffer_.get_buffer(), sizeof(GpuTransformedVertex));
        render_context_.set_index_buffer(index_ring_buffer_.get_buffer());
        render_context_.set_vertex_shader(vertex_shader_);
        render_context_.set_pixel_shader(pixel_shader_);
        render_context_.set_primitive_topology(primitive_topology_);
        render_context_.set_mode_and_textures(mode_, textures_[0], textures_[1]);
        render_context_.set_cull_mode(D3D11_CULL_NONE);
        render_context_.set_uv_offset(rf::vec2_zero_vector);

        render_context_.device_context()->DrawIndexed(num_index, start_index, 0);
    }

    static inline bool mode_uses_vertex_color(gr::Mode mode)
    {
        if (mode.get_texture_source() == gr::TEXTURE_SOURCE_NONE) {
            return true;
        }
        return mode.get_color_source() != gr::COLOR_SOURCE_TEXTURE;
    }

    static inline bool mode_uses_vertex_alpha(gr::Mode mode)
    {
        if (mode.get_texture_source() == gr::TEXTURE_SOURCE_NONE) {
            return true;
        }
        return mode.get_alpha_source() != gr::ALPHA_SOURCE_TEXTURE;
    }

    void DynamicGeometryRenderer::add_poly(int nv, const gr::Vertex **vertices, int vertex_attributes, const std::array<int, 2>& tex_handles, gr::Mode mode)
    {
        int num_index = (nv - 2) * 3;
        if (nv > batch_max_vertex || num_index > batch_max_index) {
            xlog::error("too many vertices/indices needed in dynamic geometry renderer");
            return;
        }

        std::array<int, 2> normalized_tex_handles = normalize_texture_handles_for_mode(mode, tex_handles);

        set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        set_mode(mode);
        set_textures(normalized_tex_handles);

        if (vertex_ring_buffer_.is_full(nv) || index_ring_buffer_.is_full(num_index)) {
            flush();
        }

        auto [gpu_verts, base_vertex] = vertex_ring_buffer_.alloc(nv);
        auto [gpu_ind_ptr, base_index] = index_ring_buffer_.alloc(num_index);

        bool use_vert_color = mode_uses_vertex_color(mode);
        bool use_vert_alpha = mode_uses_vertex_alpha(mode);
        rf::Color color{255, 255, 255, 255};
        if (use_vert_color && !(vertex_attributes & gr::TMAP_FLAG_RGB)) {
            color.red = gr::screen.current_color.red;
            color.green = gr::screen.current_color.green;
            color.blue = gr::screen.current_color.blue;
        }
        if (use_vert_alpha && !(vertex_attributes & gr::TMAP_FLAG_ALPHA)) {
            color.alpha = gr::screen.current_color.alpha;
        }
        // Note: gr_matrix_scale is zero before first gr_setup_3d call
        float matrix_scale_z = gr::matrix_scale.z ? gr::matrix_scale.z : 1.0f;
        for (int i = 0; i < nv; ++i) {
            const gr::Vertex& in_vert = *vertices[i];
            GpuTransformedVertex& out_vert = gpu_verts[i];
            // Set w to depth in camera space (needed for 3D rendering)
            float w = 1.0f / in_vert.sw / matrix_scale_z;
            out_vert.x = ((in_vert.sx - gr::screen.offset_x) / gr::screen.clip_width * 2.0f - 1.0f) * w;
            out_vert.y = ((in_vert.sy - gr::screen.offset_y) / gr::screen.clip_height * -2.0f + 1.0f) * w;
            out_vert.z = in_vert.sw * gr::d3d::zm * w;
            out_vert.w = w;

            if (use_vert_color && (vertex_attributes & gr::TMAP_FLAG_RGB)) {
                color.red = in_vert.r;
                color.green = in_vert.g;
                color.blue = in_vert.b;
            }
            if (use_vert_alpha && (vertex_attributes & gr::TMAP_FLAG_ALPHA)) {
                color.alpha = in_vert.a;
            }
            out_vert.diffuse = pack_color(color);
            out_vert.u0 = in_vert.u1;
            out_vert.v0 = in_vert.v1;
            //xlog::info("vert[%d]: pos <%.2f %.2f %.2f> uv <%.2f %.2f>", i, out_vert.x, out_vert.y, in_vert.sw, out_vert.u0, out_vert.v0);

            if (i >= 2) {
                *(gpu_ind_ptr++) = base_vertex;
                *(gpu_ind_ptr++) = base_vertex + i - 1;
                *(gpu_ind_ptr++) = base_vertex + i;
            }
        }
    }

    void DynamicGeometryRenderer::add_line(const gr::Vertex **vertices, rf::gr::Mode mode)
    {
        set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        set_mode(mode);
        if (vertex_ring_buffer_.is_full(2) || index_ring_buffer_.is_full(2)) {
            flush();
        }

        auto [gpu_verts, base_vertex] = vertex_ring_buffer_.alloc(2);
        auto [gpu_ind_ptr, base_index] = index_ring_buffer_.alloc(2);

        // Note: gr_matrix_scale is zero before first gr_setup_3d call
        float matrix_scale_z = gr::matrix_scale.z ? gr::matrix_scale.z : 1.0f;
        for (int i = 0; i < 2; ++i) {
            const gr::Vertex& in_vert = *vertices[i];
            GpuTransformedVertex& out_vert = gpu_verts[i];
            out_vert.x = (in_vert.sx - gr::screen.offset_x) / gr::screen.clip_width * 2.0f - 1.0f;
            out_vert.y = (in_vert.sy - gr::screen.offset_y) / gr::screen.clip_height * -2.0f + 1.0f;
            out_vert.z = in_vert.sw * gr::d3d::zm;
            // Set w to depth in camera space (needed for 3D rendering)
            out_vert.w = 1.0f / in_vert.sw / matrix_scale_z;
            out_vert.diffuse = pack_color(gr::screen.current_color);
            *(gpu_ind_ptr++) = base_vertex;
            *(gpu_ind_ptr++) = base_vertex + 1;
        }
    }

    void DynamicGeometryRenderer::bitmap(int bm_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x, bool flip_y, gr::Mode mode)
    {
        xlog::trace("Drawing bitmap");
        int bm_w, bm_h;
        bm::get_dimensions(bm_handle, &bm_w, &bm_h);

        float sx_left = static_cast<float>(x) / gr::screen.clip_width * 2.0f - 1.0f;
        float sx_right = static_cast<float>(x + w) / gr::screen.clip_width * 2.0f - 1.0f;
        float sy_top = static_cast<float>(y) / gr::screen.clip_height * -2.0f + 1.0f;
        float sy_bottom = static_cast<float>(y + h) / gr::screen.clip_height * -2.0f + 1.0f;
        float u_left = static_cast<float>(sx) / static_cast<float>(bm_w);
        float u_right = static_cast<float>(sx + sw) / static_cast<float>(bm_w);
        float v_top = static_cast<float>(sy) / static_cast<float>(bm_h);
        float v_bottom = static_cast<float>(sy + sh) / static_cast<float>(bm_h);

        // Make sure wrapped texel is not used in case of scaling with filtering enabled
        if (w != sw) {
            u_left += 0.5f / bm_w;
            u_right -= 0.5f / bm_w;
        }
        if (h != sh) {
            v_top += 0.5f / bm_h;
            v_bottom -= 0.5f / bm_h;
        }

        if (flip_x) {
            std::swap(u_left, u_right);
        }
        if (flip_y) {
            std::swap(v_top, v_bottom);
        }

        constexpr int nv = 4;
        constexpr int num_index = 6;

        set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        set_mode(mode);
        set_textures({bm_handle, -1});

        if (vertex_ring_buffer_.is_full(nv) || index_ring_buffer_.is_full(num_index)) {
            flush();
        }

        auto [gpu_verts, base_vertex] = vertex_ring_buffer_.alloc(nv);
        auto [gpu_ind_ptr, base_index] = index_ring_buffer_.alloc(num_index);
        int diffuse = pack_color(gr::screen.current_color);

        for (int i = 0; i < nv; ++i) {
            GpuTransformedVertex& gpu_vert = gpu_verts[i];
            gpu_vert.x = (i == 0 || i == 3) ? sx_left : sx_right;
            gpu_vert.y = (i == 0 || i == 1) ? sy_top : sy_bottom;
            gpu_vert.z = 1.0f;
            gpu_vert.w = 1.0f;
            gpu_vert.diffuse = diffuse;
            gpu_vert.u0 = (i == 0 || i == 3) ? u_left : u_right;
            gpu_vert.v0 = (i == 0 || i == 1) ? v_top : v_bottom;
        }
        *(gpu_ind_ptr++) = base_vertex;
        *(gpu_ind_ptr++) = base_vertex + 1;
        *(gpu_ind_ptr++) = base_vertex + 2;
        *(gpu_ind_ptr++) = base_vertex;
        *(gpu_ind_ptr++) = base_vertex + 2;
        *(gpu_ind_ptr++) = base_vertex + 3;
    }
}