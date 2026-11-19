/* libsmb2 stub: FFmpeg 的 smb:// 协议链接需要这些符号，但本移植只播 romfs 本地文件，
 * 不会走 SMB。提供最小实现让 so 能正常加载（否则 dlopen 报 symbol not found）。 */
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

void *smb2_init_context(void) { return NULL; }
void smb2_destroy_context(void *c) { (void)c; }
void *smb2_parse_url(void *c, const char *url) { (void)c; (void)url; return NULL; }
void smb2_destroy_url(void *u) { (void)u; }
int smb2_connect_share(void *c, const char *srv, const char *share, const char *user) {
    (void)c; (void)srv; (void)share; (void)user; return -1;
}
int smb2_disconnect_share(void *c) { (void)c; return -1; }
void *smb2_open(void *c, const char *path, int flags) { (void)c; (void)path; (void)flags; return NULL; }
int smb2_close(void *c, void *fh) { (void)c; (void)fh; return -1; }
ssize_t smb2_read(void *c, void *fh, void *buf, size_t count) { (void)c; (void)fh; (void)buf; (void)count; return -1; }
ssize_t smb2_write(void *c, void *fh, const void *buf, size_t count) { (void)c; (void)fh; (void)buf; (void)count; return -1; }
int64_t smb2_lseek(void *c, void *fh, int64_t offset, int whence, int64_t *current) {
    (void)c; (void)fh; (void)offset; (void)whence; if (current) *current = 0; return -1;
}
void *smb2_opendir(void *c, const char *path) { (void)c; (void)path; return NULL; }
void smb2_closedir(void *c, void *d) { (void)c; (void)d; }
void *smb2_readdir(void *c, void *d) { (void)c; (void)d; return NULL; }
int smb2_stat(void *c, const char *path, void *st) { (void)c; (void)path; (void)st; return -1; }
int smb2_unlink(void *c, const char *path) { (void)c; (void)path; return -1; }
int smb2_rename(void *c, const char *old, const char *newname) { (void)c; (void)old; (void)newname; return -1; }
void smb2_set_authentication(void *c, const char *creds) { (void)c; (void)creds; }
void smb2_set_domain(void *c, const char *d) { (void)c; (void)d; }
void smb2_set_password(void *c, const char *p) { (void)c; (void)p; }
void smb2_set_seal(void *c, int seal) { (void)c; (void)seal; }
void smb2_set_timeout(void *c, int t) { (void)c; (void)t; }
void smb2_set_sign(void *c, int s) { (void)c; (void)s; }
void smb2_set_security_mode(void *c, uint16_t m) { (void)c; (void)m; }
char *smb2_get_error(void *c) { (void)c; return (char *)"smb2 stub"; }
