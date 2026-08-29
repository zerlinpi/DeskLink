DeskLink Browser Controller
===========================

The v1.0.0 Web package contains the controller source because Vite embeds network
endpoints at build time. This avoids shipping a misleading public bundle that is
hard-coded to localhost.

Production build
----------------

1. Copy .env.restrictive.example to .env.local.
2. Configure your public WSS/HTTPS/STUN/TURN endpoints and controller-session URL.
3. From the app directory run:

     npm install
     npm run build

4. Serve dist/ behind HTTPS. Browser clipboard, Web Crypto, fullscreen, Wake Lock
   and other production APIs work best from a secure context.

Important
---------

- Do not embed the coturn REST shared secret in the browser.
- Do not embed a long-lived device credential or unattended Access Code in the
  browser bundle.
- Production controllers should use VITE_CONTROLLER_SESSION_URL and short-lived
  target-scoped controller sessions.
- WSS/HTTPS termination and TURN placement are deployment responsibilities.

v1.1 roadmap
------------

A runtime desklink-config.js loader is planned so the same prebuilt dist/ can be
moved between environments without rebuilding.
