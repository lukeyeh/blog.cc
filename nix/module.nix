# NixOS module: runs the server as a systemd service.
#
#   services.blog-server = {
#     enable = true;
#     root = ./www;
#   };
#
# The server speaks plain HTTP and listens on localhost by default. Put
# something that terminates TLS in front of it, such as Caddy or a Cloudflare
# Tunnel.
self:
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.services.blog-server;
in
{
  options.services.blog-server = {
    enable = lib.mkEnableOption "the blog HTTP server";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "blog-server from this flake";
      description = "The server package to run.";
    };

    root = lib.mkOption {
      type = lib.types.path;
      example = lib.literalExpression "./www";
      description = ''
        Directory of static files to serve.

        The server reads the files into memory when it starts and does not
        notice later changes. Given as a path in your configuration, as in the
        example, the directory is copied into the Nix store, so changing the
        site changes the path and NixOS restarts the service on the next
        deploy. Given as a string such as "/var/www", it is used in place:
        the service's user must be able to read it, and you must restart the
        service yourself after changing it.
      '';
    };

    address = lib.mkOption {
      type = lib.types.str;
      default = "127.0.0.1";
      description = ''
        IPv4 address to listen on. The default accepts connections from this
        machine only, which suits running behind a reverse proxy. Use
        "0.0.0.0" to accept them from anywhere.
      '';
    };

    port = lib.mkOption {
      type = lib.types.port;
      default = 8080;
      description = "Port to listen on.";
    };

    openFirewall = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Whether to open `port` in the firewall.";
    };

    ioBackend = lib.mkOption {
      type = lib.types.enum [
        "auto"
        "io_uring"
        "epoll"
      ];
      default = "auto";
      description = ''
        How the server does network I/O. "auto" uses io_uring where the
        system allows it and epoll otherwise.
      '';
    };

    logRequests = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Whether to log every request to the journal. Off by default because it
        slows the server down considerably.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.services.blog-server = {
      description = "Blog HTTP server";
      wantedBy = [ "multi-user.target" ];
      after = [ "network.target" ];

      serviceConfig = {
        ExecStart = lib.concatStringsSep " " (
          [
            (lib.getExe cfg.package)
            "--address=${cfg.address}"
            "--port=${toString cfg.port}"
            "--root=${cfg.root}"
            "--io_backend=${cfg.ioBackend}"
          ]
          ++ lib.optional cfg.logRequests "--v=1"
        );
        Restart = "on-failure";

        # The server needs to read its files and use the network, and nothing
        # else. It runs as a user that exists only while it is running.
        DynamicUser = true;
        # Ports below 1024 are reserved for root unless this is granted.
        AmbientCapabilities = lib.mkIf (cfg.port < 1024) [ "CAP_NET_BIND_SERVICE" ];
        CapabilityBoundingSet = if cfg.port < 1024 then [ "CAP_NET_BIND_SERVICE" ] else [ "" ];
        NoNewPrivileges = true;
        ProtectSystem = "strict";
        ProtectHome = true;
        PrivateTmp = true;
        PrivateDevices = true;
        ProtectKernelTunables = true;
        ProtectKernelModules = true;
        ProtectControlGroups = true;
        RestrictAddressFamilies = [ "AF_INET" ];
        RestrictNamespaces = true;
        LockPersonality = true;
        MemoryDenyWriteExecute = true;
        # No SystemCallFilter: the server is built on io_uring, and a filter
        # that omitted its system calls would stop it from starting.
      };
    };

    networking.firewall.allowedTCPPorts = lib.mkIf cfg.openFirewall [ cfg.port ];
  };
}
