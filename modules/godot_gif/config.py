def can_build(env, platform):
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "GIFPlayer",
        "GIFReader",
        "GIFTexture",
        "GIFWriter",
        "ResourceImporterGIFTexture",
    ]


def get_doc_path():
    return "doc_classes"
