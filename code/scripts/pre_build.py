Import("env")
import os

def pre_build_action(source, target, env):
    """
    Pre-build script for environment setup
    """
    board_model = None
    for flag in env.get("BUILD_FLAGS", []):
        if "BOARD_MODEL=" in flag:
            board_model = flag.split("=")[1]
            break

    if board_model:
        print(f"[Geogram] Building for board model: {board_model}")

    # Ensure firmware output directory exists
    firmware_dir = os.path.join(env.subst("$PROJECT_DIR"), "firmware")
    os.makedirs(firmware_dir, exist_ok=True)

env.AddPreAction("buildprog", pre_build_action)
