from slic3r_api import PluginBase, STEP_POST_SLICING


class ExternalPackageRootPlugin(PluginBase):
    def __init__(self):
        super().__init__(
            "python.external.package_root",
            STEP_POST_SLICING,
            name="External Python package root test",
            description="No-op plugin loaded from its own installed package.",
            priority=100001,
        )

    def run(self, run_ctx_address):
        return None


def register_plugin(api):
    return ExternalPackageRootPlugin()
