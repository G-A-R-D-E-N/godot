def can_build(env, platform):
    return env["rendering_device"]


def get_opts(platform):
    from SCons.Variables import BoolVariable

    return [
        BoolVariable(
            "gi_restir_di_lab",
            "Build the direct resampler's lab code: sampler lab, lab settings, lab log, counting variants",
            False,
        ),
    ]


def configure(env):
    pass


def is_enabled():
    return True
