// DawControlLook — the dark look for whatever control the kit has not
// replaced (plan M1.3). Menus, scrollbars, tabs, alerts and any stock
// BButton/BTextControl still in a window draw through here, so the app has
// one look instead of a dark panel with light islands on it.
//
// Haiku-only, and the API is private/experimental: the whole class is the
// shadow of BPrivate::BControlLook, so a Haiku change that adds a virtual
// breaks this file loudly (which is what we want).
#pragma once

#include <ControlLook.h>

namespace daw {

class DawControlLook : public BPrivate::BControlLook {
public:
    DawControlLook();
    virtual ~DawControlLook();

    virtual BAlignment DefaultLabelAlignment() const override;

    virtual float DefaultLabelSpacing() const override;

    virtual float DefaultItemSpacing() const override;

    virtual uint32 Flags(BControl* control) const override;

    virtual void DrawButtonFrame(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, const rgb_color& background, uint32 flags, uint32 borders) override;

    virtual void DrawButtonFrame(BView* view, BRect& rect, const BRect& updateRect, float radius, const rgb_color& base, const rgb_color& background, uint32 flags, uint32 borders) override;

    virtual void DrawButtonFrame(BView* view, BRect& rect, const BRect& updateRect, float leftTopRadius, float rightTopRadius, float leftBottomRadius, float rightBottomRadius, const rgb_color& base, const rgb_color& background, uint32 flags, uint32 borders) override;

    virtual void DrawButtonBackground(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders, orientation orientation) override;

    virtual void DrawButtonBackground(BView* view, BRect& rect, const BRect& updateRect, float radius, const rgb_color& base, uint32 flags, uint32 borders, orientation orientation) override;

    virtual void DrawButtonBackground(BView* view, BRect& rect, const BRect& updateRect, float leftTopRadius, float rightTopRadius, float leftBottomRadius, float rightBottomRadius, const rgb_color& base, uint32 flags, uint32 borders, orientation orientation) override;

    virtual void DrawMenuBarBackground(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders) override;

    virtual void DrawMenuFieldFrame(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, const rgb_color& background, uint32 flags, uint32 borders) override;

    virtual void DrawMenuFieldFrame(BView* view, BRect& rect, const BRect& updateRect, float radius, const rgb_color& base, const rgb_color& background, uint32 flags, uint32 borders) override;

    virtual void DrawMenuFieldFrame(BView* view, BRect& rect, const BRect& updateRect, float leftTopRadius, float rightTopRadius, float leftBottomRadius, float rightBottomRadius, const rgb_color& base, const rgb_color& background, uint32 flags, uint32 borders) override;

    virtual void DrawMenuFieldBackground(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, bool popupIndicator, uint32 flags) override;

    virtual void DrawMenuFieldBackground(BView* view, BRect& rect, const BRect& updateRect, float radius, const rgb_color& base, bool popupIndicator, uint32 flags) override;

    virtual void DrawMenuFieldBackground(BView* view, BRect& rect, const BRect& updateRect, float leftTopRadius, float rightTopRadius, float leftBottomRadius, float rightBottomRadius, const rgb_color& base, bool popupIndicator, uint32 flags) override;

    virtual void DrawMenuFieldBackground(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders) override;

    virtual void DrawMenuBackground(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders) override;

    virtual void DrawMenuItemBackground(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders) override;

    virtual void DrawStatusBar(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, const rgb_color& barColor, float progressPosition) override;

    virtual void DrawCheckBox(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags) override;

    virtual void DrawRadioButton(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags) override;

    virtual void DrawScrollBarBackground(BView* view, BRect& rect1, BRect& rect2, const BRect& updateRect, const rgb_color& base, uint32 flags, orientation orientation) override;

    virtual void DrawScrollBarBackground(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, orientation orientation) override;

    virtual void DrawScrollViewFrame(BView* view, BRect& rect, const BRect& updateRect, BRect verticalScrollBarFrame, BRect horizontalScrollBarFrame, const rgb_color& base, border_style borderStyle, uint32 flags, uint32 borders) override;

    virtual void DrawArrowShape(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 direction, uint32 flags, float tint) override;

    virtual rgb_color SliderBarColor(const rgb_color& base) override;

    virtual void DrawSliderBar(BView* view, BRect rect, const BRect& updateRect, const rgb_color& base, rgb_color leftFillColor, rgb_color rightFillColor, float sliderScale, uint32 flags, orientation orientation) override;

    virtual void DrawSliderBar(BView* view, BRect rect, const BRect& updateRect, const rgb_color& base, rgb_color fillColor, uint32 flags, orientation orientation) override;

    virtual void DrawSliderThumb(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, orientation orientation) override;

    virtual void DrawSliderTriangle(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, orientation orientation) override;

    virtual void DrawSliderTriangle(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, const rgb_color& fill, uint32 flags, orientation orientation) override;

    virtual void DrawSliderHashMarks(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, int32 count, hash_mark_location location, uint32 flags, orientation orientation) override;

    virtual void DrawActiveTab(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders, uint32 side, int32 index, int32 selected, int32 first, int32 last) override;

    virtual void DrawInactiveTab(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders, uint32 side, int32 index, int32 selected, int32 first, int32 last) override;

    virtual void DrawSplitter(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, orientation orientation, uint32 flags, uint32 borders) override;

    virtual void DrawBorder(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, border_style borderStyle, uint32 flags, uint32 borders) override;

    virtual void DrawRaisedBorder(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders) override;

    virtual void DrawGroupFrame(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 borders) override;

    virtual void DrawTextControlBorder(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders) override;

    virtual void DrawLabel(BView* view, const char* label, BRect rect, const BRect& updateRect, const rgb_color& base, uint32 flags, const rgb_color* textColor) override;

    virtual void DrawLabel(BView* view, const char* label, BRect rect, const BRect& updateRect, const rgb_color& base, uint32 flags, const BAlignment& alignment, const rgb_color* textColor) override;

    virtual void DrawLabel(BView* view, const char* label, const rgb_color& base, uint32 flags, const BPoint& where, const rgb_color* textColor) override;

    virtual void DrawLabel(BView* view, const char* label, const BBitmap* icon, BRect rect, const BRect& updateRect, const rgb_color& base, uint32 flags, const BAlignment& alignment, const rgb_color* textColor) override;

    virtual void GetFrameInsets(frame_type frameType, uint32 flags, float& _left, float& _top, float& _right, float& _bottom) override;

    virtual void GetBackgroundInsets( background_type backgroundType, uint32 flags, float& _left, float& _top, float& _right, float& _bottom) override;

    virtual void DrawButtonWithPopUpBackground(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders, orientation orientation) override;

    virtual void DrawButtonWithPopUpBackground(BView* view, BRect& rect, const BRect& updateRect, float radius, const rgb_color& base, uint32 flags, uint32 borders, orientation orientation) override;

    virtual void DrawButtonWithPopUpBackground(BView* view, BRect& rect, const BRect& updateRect, float leftTopRadius, float rightTopRadius, float leftBottomRadius, float rightBottomRadius, const rgb_color& base, uint32 flags, uint32 borders, orientation orientation) override;

    virtual void DrawTabFrame(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, uint32 borders, border_style borderStyle, uint32 side) override;

    virtual void DrawScrollBarButton(BView* view, BRect rect, const BRect& updateRect, const rgb_color& base, const rgb_color& text, uint32 flags, int32 direction, orientation orientation, bool down) override;

    virtual void DrawScrollBarThumb(BView* view, BRect& rect, const BRect& updateRect, const rgb_color& base, uint32 flags, orientation orientation, uint32 knobStyle) override;

    virtual void DrawScrollBarBorder(BView* view, BRect rect, const BRect& updateRect, const rgb_color& base, uint32 flags, orientation orientation) override;

    virtual float GetScrollBarWidth( orientation orientation) override;




private:
    // Shared pieces: a filled rounded rect, a stroked one, and the frame the
    // button/menu-field frames are built from.
    static void FillRounded(BView* view, const BRect& rect, float radius,
                            const rgb_color& color);
    static void StrokeRounded(BView* view, const BRect& rect, float radius,
                              const rgb_color& color);
    static void DrawFrame(BView* view, BRect& rect, float radius,
                          const rgb_color& background, uint32 flags,
                          const rgb_color& borderColor);
    static void DrawArrow(BView* view, const BRect& rect, uint32 direction,
                          const rgb_color& color);
    static void DrawPopupIndicator(BView* view, const BRect& rect, bool on);
};

} // namespace daw
