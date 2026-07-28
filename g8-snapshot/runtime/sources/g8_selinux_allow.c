int selinux_check_access(const char *scon, const char *tcon, const char *class_name, const char *perm, void *aux) {
  (void)scon;
  (void)tcon;
  (void)class_name;
  (void)perm;
  (void)aux;
  return 0;
}
