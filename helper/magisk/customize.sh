#!/system/bin/sh

# Magisk's installer does not preserve executable bits from every ZIP creator.
# Apply the module's narrow runtime permissions explicitly at install time.
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/bin/ivrdroid-helper" 0 0 0755
