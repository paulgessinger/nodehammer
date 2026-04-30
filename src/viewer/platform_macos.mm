#include "platform_macos.hpp"

#include <sokol_app.h>

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include <algorithm>

@interface NodehammerMacWindowBridge : NSObject {
  @public
    nodehammer::viewer::platform::MacWindowCallbacks callbacks;
    BOOL drag_active;
    CGFloat last_magnification;
}
- (instancetype)initWithCallbacks:(nodehammer::viewer::platform::MacWindowCallbacks)callbacks;
- (void)magnify:(NSMagnificationGestureRecognizer *)recognizer;
- (void)updateDrag:(id<NSDraggingInfo>)sender active:(BOOL)active;
- (void)clearDrag;
@end

namespace nodehammer::viewer::platform {
namespace {

char kBridgeAssociationKey;
IMP g_original_dragging_entered = nullptr;
IMP g_original_dragging_updated = nullptr;
IMP g_original_dragging_exited = nullptr;
IMP g_original_perform_drag_operation = nullptr;

NodehammerMacWindowBridge *bridgeForView(id view) {
    return static_cast<NodehammerMacWindowBridge *>(
        objc_getAssociatedObject(view, &kBridgeAssociationKey));
}

float dpiScale() { return static_cast<float>(sapp_dpi_scale()); }

void publishChrome(NSWindow *window, MacWindowCallbacks callbacks) {
    if (window == nil || callbacks.set_chrome == nullptr) {
        return;
    }

    WindowChromeState chrome{};
    chrome.titlebar_hidden = true;
    chrome.traffic_lights_overlap_content = true;
    chrome.content_top_inset = 32.f * dpiScale();
    chrome.content_left_inset = 88.f * dpiScale();

    NSButton *close_button = [window standardWindowButton:NSWindowCloseButton];
    NSButton *zoom_button = [window standardWindowButton:NSWindowZoomButton];
    if (close_button != nil && zoom_button != nil) {
        NSView *button_container = [close_button superview];
        NSRect close_frame = [button_container convertRect:[close_button frame] toView:nil];
        NSRect zoom_frame = [button_container convertRect:[zoom_button frame] toView:nil];
        NSRect union_frame = NSUnionRect(close_frame, zoom_frame);
        chrome.content_left_inset = static_cast<float>(NSMaxX(union_frame) + 12.0) * dpiScale();
        chrome.content_top_inset = static_cast<float>(NSHeight(union_frame) + 20.0) * dpiScale();
    }

    callbacks.set_chrome(callbacks.user, chrome);
}

DragHoverState dragStateFor(id<NSDraggingInfo> sender, bool active) {
    DragHoverState state{};
    state.supported = true;
    state.active = active;
    NSPasteboard *pasteboard = [sender draggingPasteboard];
    NSArray<Class> *classes = @[ [NSURL class] ];
    NSDictionary *options = @{NSPasteboardURLReadingFileURLsOnlyKey : @YES};
    state.file_like = [pasteboard canReadObjectForClasses:classes options:options];
    state.file_count = active ? 1 : 0;

    NSPoint location = [sender draggingLocation];
    const float scale = dpiScale();
    state.x = static_cast<float>(location.x) * scale;
    state.y = static_cast<float>(sapp_height() - location.y * scale);
    return state;
}

NSDragOperation nhDraggingEntered(id self, SEL, id<NSDraggingInfo> sender) {
    if (auto *bridge = bridgeForView(self)) {
        [bridge updateDrag:sender active:YES];
    }
    if (g_original_dragging_entered != nullptr) {
        using Fn = NSDragOperation (*)(id, SEL, id<NSDraggingInfo>);
        return reinterpret_cast<Fn>(g_original_dragging_entered)(self, @selector(draggingEntered:),
                                                                 sender);
    }
    return NSDragOperationCopy;
}

NSDragOperation nhDraggingUpdated(id self, SEL, id<NSDraggingInfo> sender) {
    if (auto *bridge = bridgeForView(self)) {
        [bridge updateDrag:sender active:YES];
    }
    if (g_original_dragging_updated != nullptr) {
        using Fn = NSDragOperation (*)(id, SEL, id<NSDraggingInfo>);
        return reinterpret_cast<Fn>(g_original_dragging_updated)(self, @selector(draggingUpdated:),
                                                                 sender);
    }
    return NSDragOperationCopy;
}

void nhDraggingExited(id self, SEL, id<NSDraggingInfo> sender) {
    if (auto *bridge = bridgeForView(self)) {
        [bridge clearDrag];
    }
    if (g_original_dragging_exited != nullptr) {
        using Fn = void (*)(id, SEL, id<NSDraggingInfo>);
        reinterpret_cast<Fn>(g_original_dragging_exited)(self, @selector(draggingExited:), sender);
    }
}

BOOL nhPerformDragOperation(id self, SEL, id<NSDraggingInfo> sender) {
    if (auto *bridge = bridgeForView(self)) {
        [bridge clearDrag];
    }
    if (g_original_perform_drag_operation != nullptr) {
        using Fn = BOOL (*)(id, SEL, id<NSDraggingInfo>);
        return reinterpret_cast<Fn>(g_original_perform_drag_operation)(
            self, @selector(performDragOperation:), sender);
    }
    return NO;
}

void replaceDragMethod(Class cls, SEL selector, IMP replacement, IMP *original, const char *types) {
    Method method = class_getInstanceMethod(cls, selector);
    if (method != nullptr) {
        if (*original == nullptr) {
            *original = method_getImplementation(method);
        }
        method_setImplementation(method, replacement);
    } else {
        class_addMethod(cls, selector, replacement, types);
    }
}

void installDragHooks(NSView *view) {
    Class cls = [view class];
    replaceDragMethod(cls, @selector(draggingEntered:), reinterpret_cast<IMP>(nhDraggingEntered),
                      &g_original_dragging_entered, "Q@:@");
    replaceDragMethod(cls, @selector(draggingUpdated:), reinterpret_cast<IMP>(nhDraggingUpdated),
                      &g_original_dragging_updated, "Q@:@");
    replaceDragMethod(cls, @selector(draggingExited:), reinterpret_cast<IMP>(nhDraggingExited),
                      &g_original_dragging_exited, "v@:@");
    replaceDragMethod(cls, @selector(performDragOperation:),
                      reinterpret_cast<IMP>(nhPerformDragOperation),
                      &g_original_perform_drag_operation, "B@:@");
}

} // namespace

float macDpiScale() { return dpiScale(); }
DragHoverState macDragStateFor(id<NSDraggingInfo> sender, bool active) {
    return dragStateFor(sender, active);
}

} // namespace nodehammer::viewer::platform

@implementation NodehammerMacWindowBridge

- (instancetype)initWithCallbacks:
    (nodehammer::viewer::platform::MacWindowCallbacks)incoming_callbacks {
    self = [super init];
    if (self != nil) {
        callbacks = incoming_callbacks;
        drag_active = NO;
        last_magnification = 0.0;
    }
    return self;
}

- (void)magnify:(NSMagnificationGestureRecognizer *)recognizer {
    using namespace nodehammer::viewer::platform;
    if (callbacks.push_gesture == nullptr) {
        return;
    }

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
    callbacks.push_gesture(callbacks.user, event);
}

- (void)updateDrag:(id<NSDraggingInfo>)sender active:(BOOL)active {
    using namespace nodehammer::viewer::platform;
    if (callbacks.set_drag_hover == nullptr) {
        return;
    }
    drag_active = active;
    DragHoverState state = macDragStateFor(sender, active);
    callbacks.set_drag_hover(callbacks.user, state);
}

- (void)clearDrag {
    using namespace nodehammer::viewer::platform;
    if (callbacks.set_drag_hover == nullptr || !drag_active) {
        return;
    }
    drag_active = NO;
    DragHoverState state{};
    state.supported = true;
    callbacks.set_drag_hover(callbacks.user, state);
}

@end

namespace nodehammer::viewer::platform {

void attachMacWindow(const WindowCustomizationRequest &request, MacWindowCallbacks callbacks) {
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
        [window setStyleMask:([window styleMask] | NSWindowStyleMaskFullSizeContentView)];
        [window setTitleVisibility:NSWindowTitleHidden];
        [window setTitlebarAppearsTransparent:YES];
        [window setMovableByWindowBackground:YES];
        publishChrome(window, callbacks);
    }

    auto *bridge = [[NodehammerMacWindowBridge alloc] initWithCallbacks:callbacks];
    objc_setAssociatedObject(view, &kBridgeAssociationKey, bridge,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    if (request.track_platform_gestures) {
        auto *recognizer =
            [[NSMagnificationGestureRecognizer alloc] initWithTarget:bridge
                                                              action:@selector(magnify:)];
        [view addGestureRecognizer:recognizer];
    }

    if (request.track_drag_hover) {
        installDragHooks(view);
        DragHoverState state{};
        state.supported = true;
        if (callbacks.set_drag_hover != nullptr) {
            callbacks.set_drag_hover(callbacks.user, state);
        }
    }
}

void syncMacWindowState(MacWindowCallbacks callbacks) {
    NSWindow *window = (__bridge NSWindow *)sapp_macos_get_window();
    if (window != nil) {
        publishChrome(window, callbacks);
    }
}

} // namespace nodehammer::viewer::platform
