/*
 * From:
 *   https://github.com/Vuhdo/imgui_freetype.git
 */
#include <ft2build.h>
#include <math.h>
#include <stdint.h>
#include <vector>

#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_SYNTHESIS_H

#include "imgui.h"
#include "imgui_freetype.h"

#ifdef __GNUC__
// Disable unused function warnings in stb_rect_packed.h
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_PLACEMENT_NEW
#include "imgui_internal.h"

#define STBRP_ASSERT( x ) IM_ASSERT( x )
#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

// Glyph metrics:
// --------------
//
//                       xmin                     xmax
//                        |                         |
//                        |<-------- width -------->|
//                        |                         |
//              |         +-------------------------+----------------- ymax
//              |         |    ggggggggg   ggggg    |     ^        ^
//              |         |   g:::::::::ggg::::g    |     |        |
//              |         |  g:::::::::::::::::g    |     |        |
//              |         | g::::::ggggg::::::gg    |     |        |
//              |         | g:::::g     g:::::g     |     |        |
//    offsetX  -|-------->| g:::::g     g:::::g     |  offsetY     |
//              |         | g:::::g     g:::::g     |     |        |
//              |         | g::::::g    g:::::g     |     |        |
//              |         | g:::::::ggggg:::::g     |     |        |
//              |         |  g::::::::::::::::g     |     |      height
//              |         |   gg::::::::::::::g     |     |        |
//  baseline ---*---------|---- gggggggg::::::g-----*--------      |
//            / |         |             g:::::g     |              |
//     origin   |         | gggggg      g:::::g     |              |
//              |         | g:::::gg   gg:::::g     |              |
//              |         |  g::::::ggg:::::::g     |              |
//              |         |   gg:::::::::::::g      |              |
//              |         |     ggg::::::ggg        |              |
//              |         |         gggggg          |              v
//              |         +-------------------------+----------------- ymin
//              |                                   |
//              |------------- advanceX ----------->|

// From SDL_ttf: Handy routines for converting from fixed point
#define FT_FLOOR( X ) ( ( X & -64 ) / 64 )
#define FT_CEIL( X )  ( ( ( X + 63 ) & -64 ) / 64 )

// Rasterized glyph image (8-bit alpha coverage).
struct GlyphBitmap
{
    static const uint32_t MaxWidth = 256;
    static const uint32_t MaxHeight = 256;

    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t grayscale[ MaxWidth * MaxHeight ];
};

// A structure that describe a glyph.
struct GlyphInfo
{
    float width;    // Glyph's width in pixels.
    float height;   // Glyph's height in pixels.
    float offsetX;  // The distance from the origin ("pen position") to the left of the glyph.
    float offsetY;  // The distance from the origin to the top of the glyph. This is usually a value < 0.
    float advanceX; // The distance from the origin to the origin of the next glyph. This is usually a value > 0.

    GlyphBitmap bmp;
};

//
//  FreeType glyph rasterizer.
//
class FreeTypeFont
{
public:
    FreeTypeFont() {}
    ~FreeTypeFont();

    // Initialize from an external data buffer.
    // Doesn't copy data, and you must ensure it stays valid up to this object lifetime.
    void Init( ImFontConfig &cfg );

    // Generate glyph image.
    bool RasterizeGlyph( uint32_t codepoint, GlyphInfo &glyphInfo );

public:
    // The pixel extents above the baseline in pixels (typically positive).
    float m_ascender;
    // The extents below the baseline in pixels (typically negative).
    float m_descender;
    // This field gives the maximum horizontal cursor advance for all glyphs in the font.
    float m_maxAdvanceWidth;

    bool m_bold;
    bool m_oblique;
    FT_Int32 m_LoadFlags;

    FT_Library m_library = nullptr;
    FT_Face m_face = nullptr;
};

FreeTypeFont::~FreeTypeFont()
{
    if ( m_face )
    {
        FT_Done_Face( m_face );
        m_face = nullptr;

        FT_Done_FreeType( m_library );
        m_library = nullptr;
    }
}

void FreeTypeFont::Init( ImFontConfig &cfg )
{
    int faceIndex = cfg.FontNo;
    float pixelHeight  = cfg.SizePixels;
    int dataSize = cfg.FontDataSize;
    const FT_Byte *data = ( const FT_Byte * )cfg.FontData;
    unsigned int flags = cfg.FreetypeFlags;

    // TODO: substitute allocator
    FT_Error error = FT_Init_FreeType( &m_library );
    IM_ASSERT( error == 0 );

    error = FT_New_Memory_Face( m_library, data, dataSize, faceIndex, &m_face );
    IM_ASSERT( error == 0 );

    error = FT_Select_Charmap( m_face, FT_ENCODING_UNICODE );
    IM_ASSERT( error == 0 );

    // I'm not sure how to deal with font sizes properly.
    // As far as I understand, currently ImGui assumes that the 'pixelHeight' is a maximum height of an any given glyph,
    // i.e. it's the sum of font's ascender and descender.
    // Seems strange to me.
    FT_Size_RequestRec req;

    req.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
    req.width = 0;
    req.height = pixelHeight * 64.0f;
    req.horiResolution = 0;
    req.vertResolution = 0;

    FT_Request_Size( m_face, &req );

    // update font info
    FT_Size_Metrics metrics = m_face->size->metrics;

    m_ascender = FT_CEIL( metrics.ascender );
    m_descender = FT_CEIL( metrics.descender );
    m_maxAdvanceWidth = FT_CEIL( metrics.max_advance );

    m_LoadFlags = FT_LOAD_NO_BITMAP;

    if ( flags & ImGuiFreeType::DisableHinting )
        m_LoadFlags |= FT_LOAD_NO_HINTING;
    if ( flags & ImGuiFreeType::ForceAutoHint )
        m_LoadFlags |= FT_LOAD_FORCE_AUTOHINT;
    if ( flags & ImGuiFreeType::NoAutoHint )
        m_LoadFlags |= FT_LOAD_NO_AUTOHINT;

    if ( flags & ImGuiFreeType::LightHinting )
        m_LoadFlags |= FT_LOAD_TARGET_LIGHT;
    else if ( flags & ImGuiFreeType::MonoHinting )
        m_LoadFlags |= FT_LOAD_TARGET_MONO;
    else
        m_LoadFlags |= FT_LOAD_TARGET_NORMAL;

    m_oblique = !!( flags & ImGuiFreeType::Oblique );
    m_bold = !!( flags & ImGuiFreeType::Bold );
}

bool FreeTypeFont::RasterizeGlyph( uint32_t codepoint, GlyphInfo &glyphInfo )
{
    uint32_t glyphIndex = FT_Get_Char_Index( m_face, codepoint );

    glyphInfo.bmp.width = 0;
    glyphInfo.bmp.height = 0;
    glyphInfo.bmp.pitch = 0;

    FT_Error error = FT_Load_Glyph( m_face, glyphIndex, m_LoadFlags );
    if ( error )
        return false;

    FT_GlyphSlot slot = m_face->glyph;

    // need an outline for this to work
    IM_ASSERT( slot->format == FT_GLYPH_FORMAT_OUTLINE );

    if ( m_oblique )
        FT_GlyphSlot_Oblique( slot );

    if ( m_bold )
        FT_GlyphSlot_Embolden( slot );

    // retrieve the glyph
    FT_Glyph glyphDesc;
    error = FT_Get_Glyph( slot, &glyphDesc );
    if ( error != 0 )
        return false;

    // rasterize
    error = FT_Glyph_To_Bitmap( &glyphDesc, FT_RENDER_MODE_NORMAL, 0, 1 );
    if ( error != 0 )
        return false;

    FT_BitmapGlyph freeTypeBitmap = ( FT_BitmapGlyph )glyphDesc;

    glyphInfo.advanceX = slot->advance.x * ( 1.0f / 64.0f );
    glyphInfo.offsetX = freeTypeBitmap->left;
    glyphInfo.offsetY = -freeTypeBitmap->top;
    glyphInfo.width = freeTypeBitmap->bitmap.width;
    glyphInfo.height = freeTypeBitmap->bitmap.rows;

    glyphInfo.bmp.width = freeTypeBitmap->bitmap.width;
    glyphInfo.bmp.height = freeTypeBitmap->bitmap.rows;
    glyphInfo.bmp.pitch = freeTypeBitmap->bitmap.pitch;

    IM_ASSERT( glyphInfo.bmp.pitch <= GlyphBitmap::MaxWidth );
    if ( freeTypeBitmap->bitmap.width && freeTypeBitmap->bitmap.buffer )
        memcpy( glyphInfo.bmp.grayscale, freeTypeBitmap->bitmap.buffer, glyphInfo.bmp.pitch * glyphInfo.bmp.height );

    // cleanup
    FT_Done_Glyph( glyphDesc );

    return true;
}

bool ImGuiFreeType::BuildFontAtlas( ImFontAtlas *atlas )
{
    IM_ASSERT( atlas->ConfigData.Size > 0 );

    atlas->TexID = NULL;
    atlas->TexWidth = atlas->TexHeight = 0;
    atlas->TexUvWhitePixel = ImVec2( 0, 0 );
    atlas->ClearTexData();

    ImVec2 maxGlyphSize = { 1.0f, 1.0f };
    std::vector< FreeTypeFont > fonts( atlas->ConfigData.Size );

    // Initialize font information early (so we can error without any cleanup) + count glyphs
    int total_glyph_count = 0;
    for ( int input_i = 0; input_i < atlas->ConfigData.Size; input_i++ )
    {
        ImFontConfig &cfg = atlas->ConfigData[ input_i ];
        FreeTypeFont &fontFace = fonts[ input_i ];

        IM_ASSERT( cfg.DstFont && ( !cfg.DstFont->IsLoaded() || cfg.DstFont->ContainerAtlas == atlas ) );

        fontFace.Init( cfg );
        maxGlyphSize.x = ImMax( maxGlyphSize.x, fontFace.m_maxAdvanceWidth );
        maxGlyphSize.y = ImMax( maxGlyphSize.y, fontFace.m_ascender - fontFace.m_descender );

        // Count glyphs
        if ( !cfg.GlyphRanges )
            cfg.GlyphRanges = atlas->GetGlyphRangesDefault();

        for ( const ImWchar *in_range = cfg.GlyphRanges; in_range[ 0 ] && in_range[ 1 ]; in_range += 2 )
            total_glyph_count += ( in_range[ 1 ] - in_range[ 0 ] ) + 1;
    }

    // Start packing. We need a known width for the skyline algorithm. Using a cheap heuristic here to decide of width. User can override TexDesiredWidth if they wish.
    // After packing is done, width shouldn't matter much, but some API/GPU have texture size limitations and increasing width can decrease height.
    if ( atlas->TexDesiredWidth > 0 )
        atlas->TexWidth = atlas->TexDesiredWidth;
    else if ( total_glyph_count > 4000 )
        atlas->TexWidth = 4096;
    else if ( total_glyph_count > 2000 )
        atlas->TexWidth = 2048;
    else if ( total_glyph_count > 1000 )
        atlas->TexWidth = 1024;
    else
        atlas->TexWidth = 512;

    // Pack our extra data rectangles first, so it will be on the upper-left corner of our texture (UV will have small values).
    ImVector< stbrp_rect > extra_rects;
    atlas->RenderCustomTexData( 0, &extra_rects );
    const int TotalRects = total_glyph_count + extra_rects.size();

    // #Vuhdo: Now, I won't do the original first pass to determine texture height, but just rough estimate.
    // Looks ugly inaccurate and excessive, but AFAIK with FreeType we actually need to render glyphs to get exact sizes.
    // Alternatively, we could just render all glyphs into a big shadow buffer, get their sizes, do the rectangle
    // packing and just copy back from the shadow buffer to the texture buffer.
    // Will give us an accurate texture height, but eat a lot of temp memory.
    // Probably no one will notice.
    float MinRectsPerRow = ceilf( ( atlas->TexWidth / ( maxGlyphSize.x + 1.0f ) ) );
    float MinRectsPerColumn = ceilf( TotalRects / MinRectsPerRow );

    atlas->TexHeight = ( int )( MinRectsPerColumn * ( maxGlyphSize.y + 1.0f ) );
    atlas->TexHeight = ImUpperPowerOfTwo( atlas->TexHeight );

    stbrp_context context;
    std::vector< stbrp_node > nodes( TotalRects );

    // Initialize a rectangle packer
    stbrp_init_target( &context, atlas->TexWidth, atlas->TexHeight, &nodes[ 0 ], TotalRects );

    // Assign packed locations to rectangles.
    stbrp_pack_rects( &context, &extra_rects[ 0 ], extra_rects.Size );

    for ( int i = 0; i < extra_rects.Size; i++ )
    {
        if ( extra_rects[ i ].was_packed )
            atlas->TexHeight = ImMax( atlas->TexHeight, extra_rects[ i ].y + extra_rects[ i ].h );
    }

    // Create texture
    atlas->TexPixelsAlpha8 = ( unsigned char * )ImGui::MemAlloc( atlas->TexWidth * atlas->TexHeight );
    memset( atlas->TexPixelsAlpha8, 0, atlas->TexWidth * atlas->TexHeight );

    // render characters, setup ImFont and glyphs for runtime
    GlyphInfo glyphInfo;

    for ( int input_i = 0; input_i < atlas->ConfigData.Size; input_i++ )
    {
        ImFontConfig &cfg = atlas->ConfigData[ input_i ];
        FreeTypeFont &fontFace = fonts[ input_i ];
        ImFont *dst_font = cfg.DstFont;

        float ascent = fontFace.m_ascender;
        float descent = fontFace.m_descender;

        if ( !cfg.MergeMode )
        {
            dst_font->ContainerAtlas = atlas;
            dst_font->ConfigData = &cfg;
            dst_font->ConfigDataCount = 0;
            dst_font->FontSize = cfg.SizePixels;
            dst_font->Ascent = ascent;
            dst_font->Descent = descent;
            dst_font->Glyphs.resize( 0 );
        }
        dst_font->ConfigDataCount++;

        float off_y = ( cfg.MergeMode && cfg.MergeGlyphCenterV ) ? ( ascent - dst_font->Ascent ) * 0.5f : 0.0f;

        // Always clear fallback so FindGlyph can return NULL. It will be set again in BuildLookupTable()
        dst_font->FallbackGlyph = NULL;

        for ( const ImWchar *in_range = cfg.GlyphRanges; in_range[ 0 ] && in_range[ 1 ]; in_range += 2 )
        {
            for ( uint32_t codepoint = in_range[ 0 ]; codepoint <= in_range[ 1 ]; ++codepoint )
            {
                if ( cfg.MergeMode && dst_font->FindGlyph( ( unsigned short )codepoint ) )
                    continue;

                fontFace.RasterizeGlyph( codepoint, glyphInfo );

                // blit to texture
                stbrp_rect rect;
                rect.w = ( uint16_t )glyphInfo.bmp.width + 1; // account for texture filtering
                rect.h = ( uint16_t )glyphInfo.bmp.height + 1;

                stbrp_pack_rects( &context, &rect, 1 );

                const uint8_t *src = glyphInfo.bmp.grayscale;
                uint8_t *dst = atlas->TexPixelsAlpha8 + rect.y * atlas->TexWidth + rect.x;
                for ( uint32_t yy = 0; yy < glyphInfo.bmp.height; ++yy )
                {
                    if ( cfg.Brighten == 0.0f )
                    {
                        memcpy( dst, src, glyphInfo.bmp.width );
                    }
                    else
                    {
                        float brighten = cfg.Brighten + 1.0f;

                        for ( uint32_t xx = 0; xx < glyphInfo.bmp.width; ++xx )
                            dst[ xx ] = std::min< uint32_t >( 255, src[ xx ] * brighten );
                    }

                    src += glyphInfo.bmp.pitch;
                    dst += atlas->TexWidth;
                }

                dst_font->Glyphs.resize( dst_font->Glyphs.Size + 1 );
                ImFont::Glyph &glyph = dst_font->Glyphs.back();

                glyph.Codepoint = ( ImWchar )codepoint;
                glyph.X0 = glyphInfo.offsetX;
                glyph.Y0 = glyphInfo.offsetY;
                glyph.X1 = glyph.X0 + glyphInfo.width;
                glyph.Y1 = glyph.Y0 + glyphInfo.height;
                glyph.U0 = rect.x / ( float )atlas->TexWidth;
                glyph.V0 = rect.y / ( float )atlas->TexHeight;
                glyph.U1 = ( rect.x + glyphInfo.width ) / ( float )atlas->TexWidth;
                glyph.V1 = ( rect.y + glyphInfo.height ) / ( float )atlas->TexHeight;
                glyph.Y0 += ( float )( int )( dst_font->Ascent + off_y + 0.5f );
                glyph.Y1 += ( float )( int )( dst_font->Ascent + off_y + 0.5f );
                glyph.XAdvance = ( glyphInfo.advanceX + cfg.GlyphExtraSpacing.x ); // Bake spacing into XAdvance

                if ( cfg.PixelSnapH )
                    glyph.XAdvance = ( float )( int )( glyph.XAdvance + 0.5f );
            }
        }

        cfg.DstFont->BuildLookupTable();
    }

    // Render into our custom data block
    atlas->RenderCustomTexData( 1, &extra_rects );

    return true;
}