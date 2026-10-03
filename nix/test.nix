# Starts a virtual machine with the module enabled and checks that the server
# really serves the site. Run with `nix flake check`.
self:
{ pkgs, ... }:

pkgs.testers.runNixOSTest {
  name = "blog-server";

  nodes.machine = {
    imports = [ self.nixosModules.default ];
    services.blog-server = {
      enable = true;
      root = ../www;
    };
    environment.systemPackages = [ pkgs.curl ];
  };

  testScript = ''
    machine.wait_for_unit("blog-server.service")
    machine.wait_for_open_port(8080)

    # The built-in endpoint, and a page from the site.
    assert machine.succeed("curl -sf http://127.0.0.1:8080/healthz") == "ok\n"
    machine.succeed("curl -sf http://127.0.0.1:8080/ | grep -q '<h1>'")
    machine.succeed("curl -sf http://127.0.0.1:8080/style.css | grep -q font-family")

    # A page that does not exist.
    status = machine.succeed(
        "curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/missing"
    )
    assert status == "404", status

    # By default it listens on localhost only, out of reach of other machines.
    machine.succeed("ss -Hltn 'sport = :8080' | grep -q '127.0.0.1:8080'")
  '';
}
