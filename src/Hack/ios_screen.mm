// what the phones screen is shaped like, so things in a corner (the labels) can stay where they can be seen
// there is probably an easier way to do this
#import <UIKit/UIKit.h>

// the round corners radius over the screens short side. 0 = square corners (or no idea)
float echo_ios_corner_ratio() {
    static float ratio = -1.f;
    if (ratio >= 0.f) return ratio;
    ratio = 0.f;
    UIScreen* screen = UIScreen.mainScreen;
    CGFloat shortSide = MIN(screen.bounds.size.width, screen.bounds.size.height);
    CGFloat radius = 0.0;
    // not a public key, but its the only place that ios says it. if john takes it away the guess below is used vv
    @try {
        radius = [[screen valueForKey:@"_displayCornerRadius"] doubleValue];
    } @catch (NSException*) {
        radius = 0.0;
    }
    if (radius <= 0.0) {
        // dynamic island gives a big safe area on 1 side, & those phones are the only ones w/ round corners
        // idek if you can run geode on an iphone 8 or whatever but its worth being smart about
        UIEdgeInsets insets = UIApplication.sharedApplication.keyWindow.safeAreaInsets;
        if (MAX(MAX(insets.left, insets.right), insets.top) > 24.0)
            radius = 50.0;
    }
    if (shortSide > 0.0) ratio = (float)(radius / shortSide);
    return ratio;
}
