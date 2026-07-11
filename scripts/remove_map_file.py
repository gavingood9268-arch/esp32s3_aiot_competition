Import("env")


def remove_map_flags(source, target, env):
    linkflags = []
    skip_next = False

    for flag in env.get("LINKFLAGS", []):
        text = str(flag)
        if skip_next:
            skip_next = False
            continue
        if text == "-Wl,-Map":
            skip_next = True
            continue
        if "-Map" in text or "firmware.map" in text:
            continue
        linkflags.append(flag)

    env.Replace(LINKFLAGS=linkflags)


env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", remove_map_flags)
