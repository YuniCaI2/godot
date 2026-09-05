{
  description = "flake for godot";
  inputs.nixpkgs.url = "git+https://github.com/NixOS/nixpkgs?ref=nixos-26.05&shallow=1";
  
  outputs = 
    {self, nixpkgs}:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; config.allowUnfree = true;};
    in
      let
      fhs = pkgs.buildFHSEnv {
        name = "godot-fhs";
        targetPkgs = pkgs: with pkgs; [
            # 基础编译工具链与加速器
            gcc
            pkg-config
            scons
            mold            
            git

            # Python (用于 CodeGen / 脚本构建)
            python3
            python3Packages.pip
            python3Packages.virtualenv

            # Vulkan 核心依赖
            vulkan-headers
            vulkan-loader
            vulkan-validation-layers
            vulkan-tools

            # OpenGL / Mesa
            libGL

            # X11 相关
            libx11
            libxrandr
            libxinerama
            libxcursor
            libxi
            libxext
            libxfixes
            libxrender      # 必需，否则 detect.py 直接报错中断

            # Wayland 相关 (Godot 4 默认启用)
            wayland
            wayland-scanner # 生成协议头文件必需
            wayland-protocols
            libdecor        # Wayland 窗口标题栏与装饰
            libxkbcommon    # 键盘按键映射，Wayland 必需

            # 音频驱动 (缺少则编译出的 Godot 没有声音)
            alsa-lib
            libpulseaudio

            # 系统集成与外设
            udev            # 手柄/控制器热插拔必需
            dbus            # 防止游戏运行时屏幕休眠
            fontconfig      # 系统字体检索支持
            speechd         # 文本转语音 TTS (可选)

        ];
        extraOutputsToInstall = [ "dev" ];
      };
      in
      {
        packages.${system} = {
          default = fhs;
          fhs = fhs;
        };

        devShells.${system}.default = fhs.env;
      };
}