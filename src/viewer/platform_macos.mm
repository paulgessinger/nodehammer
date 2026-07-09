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
    NSView *view;
    NSMagnificationGestureRecognizer *recognizer;
    BOOL drag_active;
    CGFloat last_magnification;
}
- (instancetype)initWithImpl:(nodehammer::viewer::platform::Platform::Impl *)impl;
- (void)attachToView:(NSView *)view trackDrag:(BOOL)track_drag trackGestures:(BOOL)track_gestures;
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

    explicit Impl(App &app_) : app(app_) {}
    ~Impl() {
        if (bridge != nil) {
            [bridge detach];
            [bridge release];
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

} // namespace
} // namespace nodehammer::viewer::platform

@implementation NodehammerMacWindowBridge

- (instancetype)initWithImpl:(nodehammer::viewer::platform::Platform::Impl *)incoming_impl {
    self = [super init];
    if (self != nil) {
        impl = incoming_impl;
        original_class = Nil;
        subclass = Nil;
        view = nil;
        recognizer = nil;
        drag_active = NO;
        last_magnification = 0.0;
    }
    return self;
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
    view = nil;
    original_class = Nil;
    subclass = Nil;
}

- (BOOL)beginWindowDragIfToolbarArea:(NSEvent *)event {
    if (view == nil || impl == nullptr || event == nil) {
        return NO;
    }

    NSWindow *window = [view window];
    // traffic_lights_overlap_content is only true in windowed chromeless
    // mode; in fullscreen the OS owns the title chrome via slide-down,
    // so we must not steal mouse-down to drag the (non-draggable) window.
    if (window == nil || !impl->window_request.hide_titlebar_chrome ||
        !impl->window_state.chrome.traffic_lights_overlap_content) {
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
    if (!impl->window_request.hide_titlebar_chrome) {
        return;
    }
    // Drop the chromeless treatment before the transition. With
    // FullSizeContentView + transparent titlebar, AppKit treats the
    // chrome as a translucent overlay pinned at the top — the standard
    // top-edge auto-reveal of the title bar (with traffic lights) and
    // menu bar doesn't fire. Reverting to a vanilla titled window for
    // the duration of fullscreen lets AppKit's default behavior work.
    NSWindowStyleMask mask = [win styleMask];
    mask &= ~NSWindowStyleMaskFullSizeContentView;
    [win setStyleMask:mask];
    [win setTitlebarAppearsTransparent:NO];
    [win setMovableByWindowBackground:NO];
}

- (void)windowWillExitFullScreen:(NSNotification *)notification {
    NSWindow *win = [notification object];
    if (win == nil || impl == nullptr) {
        return;
    }
    if (!impl->window_request.hide_titlebar_chrome) {
        return;
    }
    NSWindowStyleMask mask = [win styleMask];
    mask |= NSWindowStyleMaskFullSizeContentView;
    [win setStyleMask:mask];
    [win setTitlebarAppearsTransparent:YES];
    [win setMovableByWindowBackground:YES];
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
    if (!impl.window_request.hide_titlebar_chrome) {
        impl.window_state.chrome = {};
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

    // Sokol launches us as a command-line app (no .app bundle) and never
    // calls -[NSApp setMainMenu:]. Without a main menu, AppKit's "auto-show
    // menu bar on top-edge hover" in fullscreen has nothing to reveal — see
    // https://github.com/floooh/sokol-nim/issues/37. Build a minimal app
    // menu so AppKit has a menu bar to manage.
    if ([NSApp mainMenu] == nil) {
        NSMenu *menubar = [[[NSMenu alloc] init] autorelease];
        NSMenuItem *app_item = [[[NSMenuItem alloc] init] autorelease];
        [menubar addItem:app_item];
        NSMenu *app_menu = [[[NSMenu alloc] init] autorelease];
        NSString *app_name = [[NSProcessInfo processInfo] processName];
        [app_menu addItemWithTitle:[NSString stringWithFormat:@"Quit %@", app_name]
                            action:@selector(terminate:)
                     keyEquivalent:@"q"];
        [app_item setSubmenu:app_menu];
        [NSApp setMainMenu:menubar];
    }

    if (request.hide_titlebar_chrome) {
        // Chromeless windowed mode: title bar is transparent, content
        // extends behind it via FullSizeContentView, traffic lights
        // overlap the imgui content. Applied ONCE — the windowWill*
        // handlers strip and restore these around fullscreen, since
        // AppKit's top-edge auto-reveal won't work while they're set.
        [window setStyleMask:([window styleMask] | NSWindowStyleMaskFullSizeContentView)];
        [window setTitleVisibility:NSWindowTitleVisible];
        [window setTitlebarAppearsTransparent:YES];
        [window setMovableByWindowBackground:YES];
        if (@available(macOS 10.14, *)) {
            [window setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
        }
        [window setCollectionBehavior:([window collectionBehavior] |
                                       NSWindowCollectionBehaviorFullScreenPrimary)];
        macPublishChrome(window, *impl_);
    }

    impl_->bridge = [[NodehammerMacWindowBridge alloc] initWithImpl:impl_.get()];
    [impl_->bridge attachToView:view
                      trackDrag:request.track_drag_hover
                  trackGestures:request.track_platform_gestures];
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

bool Platform::hasPendingGestures() const noexcept { return !impl_->gesture_events.empty(); }

void Platform::dispatchDroppedFiles() { dispatchNativeDroppedFiles(impl_->app); }

std::optional<std::string> Platform::loadPersistentText(const std::string &key) const {
    return loadNativePersistentText(key);
}

void Platform::savePersistentText(const std::string &key, const std::string &bytes) {
    saveNativePersistentText(key, bytes);
}

void Platform::commitUrlState(const std::string & /*state_query*/,
                              const std::string & /*managed_keys*/) {
    // No browser URL on native.
}

void Platform::openUrl(const std::string & /*url*/) {
    // TODO: native URL opener intentionally deferred.
}

std::optional<std::string> Platform::saveExportedImage(const std::string &filename,
                                                       std::span<const std::byte> bytes) {
    return saveNativeExportedImage(filename, bytes);
}

void Platform::downloadArchive(const std::string &, std::span<const std::byte>) {
    // Native archives are written to a picked path (saveArchivePicker); there is
    // no browser download.
}

// Web application-mode IDB persistence has no native equivalent, so these are
// no-ops (native modes persist through their own on-disk backing).
void Platform::loadProjectBlob() {}
void Platform::saveProjectBlob(std::span<const std::byte>) {}
void Platform::clearProjectBlob() {}

void Platform::openFilePicker() { impl_->pickers.openFilePicker(); }
void Platform::openFolderPicker() { impl_->pickers.openFolderPicker(); }
void Platform::openArchivePicker() { impl_->pickers.openArchivePicker(); }
void Platform::saveArchivePicker() { impl_->pickers.saveArchivePicker(); }
void Platform::drainPickers() { impl_->pickers.drainPickers(impl_->app); }

} // namespace nodehammer::viewer::platform
