#import <Foundation/Foundation.h>

#ifdef __cplusplus
extern "C" {
#endif

NS_ASSUME_NONNULL_BEGIN
NSData *_Nullable MagatamaReadSettings(NSString *path);
BOOL MagatamaWriteSettings(NSString *path, NSDictionary<NSString *, NSString *> *changes);
NS_ASSUME_NONNULL_END

#ifdef __cplusplus
}
#endif
