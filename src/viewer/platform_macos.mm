#include <nodehammer/viewer/platform.hpp>

#include "platform_native_common.hpp"

#include <nodehammer/viewer/app.hpp>

#include <sokol_app.h>

#import <Cocoa/Cocoa.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <print>
#include <string>
#include <utility>
#include <vector>

namespace nodehammer::viewer::platform {
float macDpiScale();
DragHoverState macDragStateFor(id<NSDraggingInfo> sender, bool active);
void macPublishChrome(NSWindow *window, Platform::Impl &impl);
void macPushMagnification(Platform::Impl &impl, NSMagnificationGestureRecognizer *recognizer,
                          CGFloat &last_magnification);
} // namespace nodehammer::viewer::platform

@interface NodehammerMacWindowBridge : NSObject {
  @public
    nodehammer::viewer::platform::Platform::Impl *impl;
    Class original_class;
    Class subclass;
    Class original_delegate_class;
    Class delegate_subclass;
    id delegate_object;
    NSView *view;
    NSMagnificationGestureRecognizer *recognizer;
    BOOL drag_active;
    CGFloat last_magnification;
}
- (instancetype)initWithImpl:(nodehammer::viewer::platform::Platform::Impl *)impl;
- (void)attachToView:(NSView *)view trackDrag:(BOOL)track_drag trackGestures:(BOOL)track_gestures;
- (void)attachToWindowDelegate:(NSWindow *)window;
- (void)detach;
- (BOOL)beginWindowDragIfToolbarArea:(NSEvent *)event;
- (void)magnify:(NSMagnificationGestureRecognizer *)recognizer;
- (void)updateDrag:(id<NSDraggingInfo>)sender active:(BOOL)active;
- (void)clearDrag;
- (void)windowWillEnterFullScreen:(NSNotification *)notification;
- (void)windowWillExitFullScreen:(NSNotification *)notification;
@end

namespace nodehammer::viewer::platform {

struct Platform::Impl {
    App &app;
    NativePickerState pickers;
    WindowCustomizationRequest window_request;
    PlatformWindowState window_state;
    std::vector<PlatformGestureEvent> gesture_events;
    NodehammerMacWindowBridge *bridge{nil};
    NSToolbar *toolbar{nil};

    explicit Impl(App &app_) : app(app_) {}
    ~Impl() {
        if (bridge != nil) {
            [bridge detach];
            [bridge release];
        }
        if (toolbar != nil) {
            [toolbar release];
        }
    }
};

namespace {

NodehammerMacWindowBridge *bridgeForObject(id self) {
    void *bridge = nullptr;
    object_getInstanceVariable(self, "_nodehammerBridge", &bridge);
    return static_cast<NodehammerMacWindowBridge *>(bridge);
}

bool superclassResponds(id self, SEL selector) {
    Class super_class = class_getSuperclass(object_getClass(self));
    return super_class != Nil && class_getInstanceMethod(super_class, selector) != nullptr;
}

NSDragOperation forwardDragOperationToSuper(id self, SEL selector, id<NSDraggingInfo> sender,
                                            NSDragOperation fallback) {
    if (!superclassResponds(self, selector)) {
        return fallback;
    }
    struct objc_super super_info{self, class_getSuperclass(object_getClass(self))};
    using Fn = NSDragOperation (*)(struct objc_super *, SEL, id<NSDraggingInfo>);
    return reinterpret_cast<Fn>(objc_msgSendSuper)(&super_info, selector, sender);
}

BOOL forwardBoolToSuper(id self, SEL selector, id<NSDraggingInfo> sender, BOOL fallback) {
    if (!superclassResponds(self, selector)) {
        return fallback;
    }
    struct objc_super super_info{self, class_getSuperclass(object_getClass(self))};
    using Fn = BOOL (*)(struct objc_super *, SEL, id<NSDraggingInfo>);
    return reinterpret_cast<Fn>(objc_msgSendSuper)(&super_info, selector, sender);
}

void forwardVoidToSuper(id self, SEL selector, id<NSDraggingInfo> sender) {
    if (!superclassResponds(self, selector)) {
        return;
    }
    struct objc_super super_info{self, class_getSuperclass(object_getClass(self))};
    using Fn = void (*)(struct objc_super *, SEL, id<NSDraggingInfo>);
    reinterpret_cast<Fn>(objc_msgSendSuper)(&super_info, selector, sender);
}

void forwardMouseEventToSuper(id self, SEL selector, NSEvent *event) {
    if (!superclassResponds(self, selector)) {
        return;
    }
    struct objc_super super_info{self, class_getSuperclass(object_getClass(self))};
    using Fn = void (*)(struct objc_super *, SEL, NSEvent *);
    reinterpret_cast<Fn>(objc_msgSendSuper)(&super_info, selector, event);
}

void nhMouseDown(id self, SEL selector, NSEvent *event) {
    if (auto *bridge = bridgeForObject(self)) {
        if ([bridge beginWindowDragIfToolbarArea:event]) {
            return;
        }
    }
    forwardMouseEventToSuper(self, selector, event);
}

NSDragOperation nhDraggingEntered(id self, SEL selector, id<NSDraggingInfo> sender) {
    if (auto *bridge = bridgeForObject(self)) {
        [bridge updateDrag:sender active:YES];
    }
    return forwardDragOperationToSuper(self, selector, sender, NSDragOperationCopy);
}

NSDragOperation nhDraggingUpdated(id self, SEL selector, id<NSDraggingInfo> sender) {
    if (auto *bridge = bridgeForObject(self)) {
        [bridge updateDrag:sender active:YES];
    }
    return forwardDragOperationToSuper(self, selector, sender, NSDragOperationCopy);
}

void nhDraggingExited(id self, SEL selector, id<NSDraggingInfo> sender) {
    if (auto *bridge = bridgeForObject(self)) {
        [bridge clearDrag];
    }
    forwardVoidToSuper(self, selector, sender);
}

BOOL nhPerformDragOperation(id self, SEL selector, id<NSDraggingInfo> sender) {
    if (auto *bridge = bridgeForObject(self)) {
        [bridge clearDrag];
    }
    return forwardBoolToSuper(self, selector, sender, NO);
}

Class makeViewSubclass(NSView *view) {
    Class original = [view class];
    std::array<char, 128> name{};
    std::snprintf(name.data(), name.size(), "%s_Nodehammer_%p", class_getName(original),
                  static_cast<void *>(view));

    Class subclass = objc_allocateClassPair(original, name.data(), 0);
    if (subclass == Nil) {
        return Nil;
    }

    class_addIvar(subclass, "_nodehammerBridge", sizeof(id),
                  static_cast<uint8_t>(__builtin_ctz(sizeof(id))), @encode(id));
    class_addMethod(subclass, @selector(draggingEntered:), reinterpret_cast<IMP>(nhDraggingEntered),
                    "Q@:@");
    class_addMethod(subclass, @selector(draggingUpdated:), reinterpret_cast<IMP>(nhDraggingUpdated),
                    "Q@:@");
    class_addMethod(subclass, @selector(draggingExited:), reinterpret_cast<IMP>(nhDraggingExited),
                    "v@:@");
    class_addMethod(subclass, @selector(performDragOperation:),
                    reinterpret_cast<IMP>(nhPerformDragOperation), "B@:@");
    class_addMethod(subclass, @selector(mouseDown:), reinterpret_cast<IMP>(nhMouseDown), "v@:@");
    objc_registerClassPair(subclass);
    return subclass;
}

NSApplicationPresentationOptions
nhWillUseFullScreenPresentationOptions(id self, SEL selector, NSWindow *window,
                                       NSApplicationPresentationOptions proposed) {
    (void)window;
    NSApplicationPresentationOptions opts = proposed;
    if (superclassResponds(self, selector)) {
        struct objc_super super_info{self, class_getSuperclass(object_getClass(self))};
        using Fn = NSApplicationPresentationOptions (*)(struct objc_super *, SEL, NSWindow *,
                                                        NSApplicationPresentationOptions);
        opts = reinterpret_cast<Fn>(objc_msgSendSuper)(&super_info, selector, window, proposed);
    }
    opts |= NSApplicationPresentationAutoHideMenuBar | NSApplicationPresentationAutoHideToolbar;
    std::println(
        stderr,
        "[viewer/mac] willUseFullScreenPresentationOptions: proposed=0x{:x} returning=0x{:x}",
        static_cast<unsigned long long>(proposed), static_cast<unsigned long long>(opts));
    return opts;
}

Class makeDelegateSubclass(id delegate) {
    Class original = [delegate class];
    std::array<char, 128> name{};
    std::snprintf(name.data(), name.size(), "%s_NodehammerDelegate_%p", class_getName(original),
                  static_cast<void *>(delegate));
    Class subclass = objc_allocateClassPair(original, name.data(), 0);
    if (subclass == Nil) {
        return Nil;
    }
    // NSApplicationPresentationOptions is NSUInteger, encoded as Q on 64-bit.
    class_addMethod(subclass, @selector(window:willUseFullScreenPresentationOptions:),
                    reinterpret_cast<IMP>(nhWillUseFullScreenPresentationOptions), "Q@:@Q");
    objc_registerClassPair(subclass);
    return subclass;
}

} // namespace
} // namespace nodehammer::viewer::platform

@implementation NodehammerMacWindowBridge

- (instancetype)initWithImpl:(nodehammer::viewer::platform::Platform::Impl *)incoming_impl {
    self = [super init];
    if (self != nil) {
        impl = incoming_impl;
        original_class = Nil;
        subclass = Nil;
        original_delegate_class = Nil;
        delegate_subclass = Nil;
        delegate_object = nil;
        view = nil;
        recognizer = nil;
        drag_active = NO;
        last_magnification = 0.0;
    }
    return self;
}

- (void)attachToWindowDelegate:(NSWindow *)window {
    id current_delegate = [window delegate];
    std::println(stderr, "[viewer/mac] attachToWindowDelegate: delegate={} class={}",
                 static_cast<const void *>(current_delegate),
                 current_delegate != nil ? class_getName([current_delegate class]) : "<nil>");
    if (current_delegate == nil) {
        return;
    }
    delegate_object = current_delegate;
    original_delegate_class = [current_delegate class];
    delegate_subclass = nodehammer::viewer::platform::makeDelegateSubclass(current_delegate);
    if (delegate_subclass != Nil) {
        object_setClass(current_delegate, delegate_subclass);
        std::println(stderr, "[viewer/mac] delegate ISA-swizzled to {}",
                     class_getName(delegate_subclass));
    }
}

- (void)attachToView:(NSView *)incoming_view
           trackDrag:(BOOL)track_drag
       trackGestures:(BOOL)track_gestures {
    if (incoming_view == nil) {
        return;
    }

    view = incoming_view;
    original_class = [view class];

    if (track_drag) {
        subclass = nodehammer::viewer::platform::makeViewSubclass(view);
        if (subclass != Nil) {
            object_setClass(view, subclass);
            object_setIvar(view, class_getInstanceVariable(subclass, "_nodehammerBridge"), self);
        }
    }

    if (track_gestures) {
        recognizer = [[NSMagnificationGestureRecognizer alloc] initWithTarget:self
                                                                       action:@selector(magnify:)];
        [view addGestureRecognizer:recognizer];
    }
}

- (void)detach {
    if (view != nil && recognizer != nil) {
        [view removeGestureRecognizer:recognizer];
        [recognizer release];
        recognizer = nil;
    }
    if (view != nil && subclass != Nil && object_getClass(view) == subclass) {
        object_setClass(view, original_class);
    }
    if (delegate_object != nil && delegate_subclass != Nil &&
        object_getClass(delegate_object) == delegate_subclass) {
        object_setClass(delegate_object, original_delegate_class);
    }
    view = nil;
    original_class = Nil;
    subclass = Nil;
    delegate_object = nil;
    original_delegate_class = Nil;
    delegate_subclass = Nil;
}

- (BOOL)beginWindowDragIfToolbarArea:(NSEvent *)event {
    if (view == nil || impl == nullptr || event == nil) {
        return NO;
    }

    NSWindow *window = [view window];
    // traffic_lights_overlap_content is only true in windowed chromeless
    // mode; in fullscreen the OS owns the title chrome via slide-down,
    // so we must not steal mouse-down to drag the (non-draggable) window.
    if (window == nil || !impl->window_state.chrome.traffic_lights_overlap_content) {
        return NO;
    }

    const NSPoint location = [view convertPoint:[event locationInWindow] fromView:nil];
    const NSRect bounds = [view bounds];
    const CGFloat toolbar_height =
        std::max<CGFloat>(36.0, impl->window_state.chrome.content_top_inset /
                                    nodehammer::viewer::platform::macDpiScale());
    if (location.y < NSMaxY(bounds) - toolbar_height) {
        return NO;
    }

    [window performWindowDragWithEvent:event];
    return YES;
}

- (void)magnify:(NSMagnificationGestureRecognizer *)incoming_recognizer {
    if (impl == nullptr) {
        return;
    }
    nodehammer::viewer::platform::macPushMagnification(*impl, incoming_recognizer,
                                                       last_magnification);
}

- (void)updateDrag:(id<NSDraggingInfo>)sender active:(BOOL)active {
    if (impl == nullptr) {
        return;
    }
    drag_active = active;
    impl->window_state.drag_hover = nodehammer::viewer::platform::macDragStateFor(sender, active);
}

- (void)clearDrag {
    if (impl == nullptr || !drag_active) {
        return;
    }
    drag_active = NO;
    nodehammer::viewer::platform::DragHoverState state{};
    state.supported = true;
    impl->window_state.drag_hover = state;
}

- (void)windowWillEnterFullScreen:(NSNotification *)notification {
    NSWindow *win = [notification object];
    if (win == nil || impl == nullptr) {
        return;
    }
    // Drop the entire chromeless treatment for fullscreen. With
    // FullSizeContentView + a toolbar + transparent titlebar, AppKit
    // treats the chrome as a translucent overlay that's pinned at the
    // top — the standard top-edge auto-reveal (menu bar AND title bar)
    // doesn't fire. Stripping back to a vanilla titled window for the
    // duration of fullscreen lets AppKit's default behavior take over.
    NSWindowStyleMask mask = [win styleMask];
    mask &= ~NSWindowStyleMaskFullSizeContentView;
    [win setStyleMask:mask];
    [win setTitlebarAppearsTransparent:NO];
    [win setMovableByWindowBackground:NO];
    [win setToolbar:nil];
    std::println(stderr, "[viewer/mac] windowWillEnterFullScreen: stripped chromeless treatment");
}

- (void)windowWillExitFullScreen:(NSNotification *)notification {
    NSWindow *win = [notification object];
    if (win == nil || impl == nullptr) {
        return;
    }
    NSWindowStyleMask mask = [win styleMask];
    mask |= NSWindowStyleMaskFullSizeContentView;
    [win setStyleMask:mask];
    [win setTitlebarAppearsTransparent:YES];
    [win setMovableByWindowBackground:YES];
    if (impl->toolbar != nil) {
        [win setToolbar:impl->toolbar];
    }
    std::println(stderr, "[viewer/mac] windowWillExitFullScreen: restored chromeless treatment");
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self detach];
    [super dealloc];
}

@end

namespace nodehammer::viewer::platform {

float macDpiScale() { return static_cast<float>(sapp_dpi_scale()); }

bool macIsFullscreen(NSWindow *window) {
    return window != nil && ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

void macPublishChrome(NSWindow *window, Platform::Impl &impl) {
    if (window == nil) {
        return;
    }
    // Style mask / titlebar attributes were applied once at attach time.
    // This path runs every frame and only computes the per-frame insets,
    // so it must not mutate window state — doing so during the fullscreen
    // animation cancels the transition.
    const bool fullscreen = macIsFullscreen(window);

    WindowChromeState chrome{};
    chrome.titlebar_transparent = true;
    // In fullscreen the system auto-hides the titlebar and slides it down
    // on top-edge hover, so there are no traffic lights overlapping the
    // content — let ImGui use the full window.
    chrome.traffic_lights_overlap_content = !fullscreen;
    if (!fullscreen) {
        chrome.content_top_inset = 32.f * macDpiScale();
        chrome.content_left_inset = 88.f * macDpiScale();

        NSButton *close_button = [window standardWindowButton:NSWindowCloseButton];
        NSButton *zoom_button = [window standardWindowButton:NSWindowZoomButton];
        if (close_button != nil && zoom_button != nil) {
            NSView *button_container = [close_button superview];
            NSRect close_frame = [button_container convertRect:[close_button frame] toView:nil];
            NSRect zoom_frame = [button_container convertRect:[zoom_button frame] toView:nil];
            NSRect union_frame = NSUnionRect(close_frame, zoom_frame);
            chrome.content_left_inset =
                static_cast<float>(NSMaxX(union_frame) + 12.0) * macDpiScale();
            chrome.content_top_inset =
                static_cast<float>(NSHeight(union_frame) + 20.0) * macDpiScale();
        }
    }

    impl.window_state.chrome = chrome;
}

DragHoverState macDragStateFor(id<NSDraggingInfo> sender, bool active) {
    DragHoverState state{};
    state.supported = true;
    state.active = active;
    NSPasteboard *pasteboard = [sender draggingPasteboard];
    NSArray<Class> *classes = @[ [NSURL class] ];
    NSDictionary *options = @{NSPasteboardURLReadingFileURLsOnlyKey : @YES};
    state.file_like = [pasteboard canReadObjectForClasses:classes options:options];
    state.file_count = active ? 1 : 0;

    NSPoint location = [sender draggingLocation];
    const float scale = macDpiScale();
    state.x = static_cast<float>(location.x) * scale;
    state.y = static_cast<float>(sapp_height() - location.y * scale);
    return state;
}

void macPushMagnification(Platform::Impl &impl, NSMagnificationGestureRecognizer *recognizer,
                          CGFloat &last_magnification) {
    PlatformGestureEvent event{};
    switch ([recognizer state]) {
    case NSGestureRecognizerStateBegan:
        last_magnification = [recognizer magnification];
        event.type = GestureType::PinchBegin;
        event.scale_delta = 1.f;
        break;
    case NSGestureRecognizerStateChanged: {
        const CGFloat magnification = [recognizer magnification];
        const CGFloat delta = magnification - last_magnification;
        last_magnification = magnification;
        event.type = GestureType::PinchUpdate;
        event.scale_delta = static_cast<float>(std::max<CGFloat>(0.05, 1.0 + delta));
        break;
    }
    case NSGestureRecognizerStateEnded:
        event.type = GestureType::PinchEnd;
        event.scale_delta = 1.f;
        break;
    case NSGestureRecognizerStateCancelled:
    case NSGestureRecognizerStateFailed:
        event.type = GestureType::PinchCancel;
        event.scale_delta = 1.f;
        break;
    default:
        return;
    }

    NSPoint location = [recognizer locationInView:[recognizer view]];
    const float scale = macDpiScale();
    event.x = static_cast<float>(location.x) * scale;
    event.y = static_cast<float>(sapp_height() - location.y * scale);
    impl.gesture_events.push_back(event);
}

Platform::Platform(App &app) : impl_(std::make_unique<Impl>(app)) {}
Platform::~Platform() = default;

void Platform::configureWindowDesc(sapp_desc & /*desc*/, const Config & /*cfg*/,
                                   const WindowCustomizationRequest &request) {
    impl_->window_request = request;
}

void Platform::attachWindow(const WindowCustomizationRequest &request) {
    impl_->window_request = request;
    impl_->window_state.supports_window_restoration = request.restore_placement;
    impl_->window_state.supports_hidden_titlebar = request.hide_titlebar_chrome;
    impl_->window_state.supports_pinch_gesture = request.track_platform_gestures;

    NSWindow *window = (__bridge NSWindow *)sapp_macos_get_window();
    NSView *view = [window contentView];
    if (window == nil || view == nil) {
        return;
    }

    if (request.restore_placement) {
        NSString *autosave_name = [NSString stringWithUTF8String:request.persistence_id.c_str()];
        [window setFrameUsingName:autosave_name force:NO];
        [window setFrameAutosaveName:autosave_name];
        [window setRestorable:YES];
    }

    if (request.hide_titlebar_chrome) {
        // Apply chromeless titlebar style ONCE. The style mask must not
        // be touched after this — toggling it while the OS is animating
        // a fullscreen transition cancels the transition.
        //
        // The empty NSToolbar is required for the fullscreen auto-reveal
        // behavior: in fullscreen, AppKit slides down the toolbar +
        // titlebar (with traffic lights) on top-edge hover. Without a
        // toolbar attached there is no surface to reveal, so the user
        // gets stuck — no way to access the menu bar or exit fullscreen.
        [window setStyleMask:([window styleMask] | NSWindowStyleMaskFullSizeContentView)];
        [window setTitleVisibility:NSWindowTitleVisible];
        [window setTitlebarAppearsTransparent:YES];
        [window setMovableByWindowBackground:YES];
        if (@available(macOS 10.14, *)) {
            [window setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
        }
        [window setCollectionBehavior:([window collectionBehavior] |
                                       NSWindowCollectionBehaviorFullScreenPrimary)];

        impl_->toolbar = [[NSToolbar alloc] initWithIdentifier:@"nodehammer.viewer.toolbar"];
        [impl_->toolbar setAllowsUserCustomization:NO];
        [impl_->toolbar setAutosavesConfiguration:NO];
        [window setToolbar:impl_->toolbar];

        macPublishChrome(window, *impl_);
    }

    impl_->bridge = [[NodehammerMacWindowBridge alloc] initWithImpl:impl_.get()];
    [impl_->bridge attachToView:view
                      trackDrag:request.track_drag_hover
                  trackGestures:request.track_platform_gestures];
    [impl_->bridge attachToWindowDelegate:window];
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:impl_->bridge
               selector:@selector(windowWillEnterFullScreen:)
                   name:NSWindowWillEnterFullScreenNotification
                 object:window];
    [center addObserver:impl_->bridge
               selector:@selector(windowWillExitFullScreen:)
                   name:NSWindowWillExitFullScreenNotification
                 object:window];

    if (request.track_drag_hover) {
        DragHoverState state{};
        state.supported = true;
        impl_->window_state.drag_hover = state;
    }
}

void Platform::handleWindowEvent(const sapp_event *ev) {
    if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        impl_->window_state.drag_hover.active = false;
    }
}

void Platform::beginFrameWindowSync() {
    NSWindow *window = (__bridge NSWindow *)sapp_macos_get_window();
    if (window != nil) {
        macPublishChrome(window, *impl_);
    }
}

const PlatformWindowState &Platform::windowState() const noexcept { return impl_->window_state; }

std::vector<PlatformGestureEvent> Platform::takeGestureEvents() {
    return std::exchange(impl_->gesture_events, {});
}

void Platform::dispatchDroppedFiles() { dispatchNativeDroppedFiles(impl_->app); }

void Platform::commitUrlState(const std::string & /*state_query*/,
                              const std::string & /*managed_keys*/) {
    // No browser URL on native.
}

void Platform::openFilePicker() { impl_->pickers.openFilePicker(); }
void Platform::openFolderPicker() { impl_->pickers.openFolderPicker(); }
void Platform::drainPickers() { impl_->pickers.drainPickers(impl_->app); }

} // namespace nodehammer::viewer::platform
