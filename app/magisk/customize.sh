#!/system/bin/sh

# Magisk installers do not consistently preserve ZIP modes. Keep the system overlay readable
# while leaving only this installer hook executable.
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/customize.sh" 0 0 0755
set_perm "$MODPATH/system/priv-app/IVRdroid/IVRdroid.apk" 0 0 0644
set_perm \
    "$MODPATH/system/etc/permissions/privapp-permissions-ai.rx1.ivrdroid.xml" \
    0 0 0644
set_perm \
    "$MODPATH/system/etc/sysconfig/ai.rx1.ivrdroid.xml" \
    0 0 0644
