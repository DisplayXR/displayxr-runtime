### Added
- [libmonado] Added checks for new hand-tracking role names for `mnd_root_get_device_from_role`
### Changed
- [libmonado] Deprecated the role name strings `"hand-tracking-[left|right]"` for `mnd_root_get_device_from_role`,
  these now map to `"hand-tracking-unobstructed-[left|right]"`, to removed in the future.
### Fixed
- [libmonado] Fixed `monado.py` using old hand-tracking role names in `Monado.get_device_roles`
