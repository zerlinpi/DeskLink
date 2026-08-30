# syntax=docker/dockerfile:1

FROM node:22-alpine AS build
WORKDIR /app

COPY package.json package-lock.json tsconfig.json index.html vite.config.ts ./
COPY src ./src
COPY public ./public
COPY scripts ./scripts

# These build arguments are compatibility/default values only. The deployable
# bundle can override public endpoint settings later through desklink-config.js
# without rebuilding the image. Authentication secrets are intentionally not
# part of that runtime configuration surface.
ARG VITE_SIGNAL_URL=/ws
ARG VITE_CONTROLLER_SESSION_URL=/api/v1/controller-session
ARG VITE_CONTROLLER_AUTH_REQUIRED=1
ARG VITE_STUN_URL=
ARG VITE_TURN_URL=
ARG VITE_TURN_TLS_URL=
ARG VITE_TURN_CREDENTIALS_URL=/api/v1/turn-credentials
ARG VITE_TURN_RUNTIME_REQUIRED=1
ARG VITE_ICE_TRANSPORT_POLICY=all

ENV VITE_SIGNAL_URL=${VITE_SIGNAL_URL} \
    VITE_CONTROLLER_SESSION_URL=${VITE_CONTROLLER_SESSION_URL} \
    VITE_CONTROLLER_AUTH_REQUIRED=${VITE_CONTROLLER_AUTH_REQUIRED} \
    VITE_STUN_URL=${VITE_STUN_URL} \
    VITE_TURN_URL=${VITE_TURN_URL} \
    VITE_TURN_TLS_URL=${VITE_TURN_TLS_URL} \
    VITE_TURN_CREDENTIALS_URL=${VITE_TURN_CREDENTIALS_URL} \
    VITE_TURN_RUNTIME_REQUIRED=${VITE_TURN_RUNTIME_REQUIRED} \
    VITE_ICE_TRANSPORT_POLICY=${VITE_ICE_TRANSPORT_POLICY}

RUN npm ci \
    && npm test \
    && npm run build

FROM nginx:alpine
COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=build /app/dist /usr/share/nginx/html

EXPOSE 80
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
  CMD wget -q -O /dev/null http://127.0.0.1/ || exit 1
