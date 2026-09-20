FROM --platform=linux/amd64 node:24.10.0-alpine@sha256:1298b727c38ebda277ae25ef5ad2ec0ab0c65c714180f22c32184d96ca303c11 AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json tsconfig.app.json tsconfig.node.json vite.config.ts index.html ./
COPY src ./src
COPY public ./public
ARG SOURCE_COMMIT=unknown
ENV VITE_SOURCE_COMMIT=${SOURCE_COMMIT}
RUN npm run build

FROM --platform=linux/amd64 nginxinc/nginx-unprivileged:1.29-alpine@sha256:8059701048a6cc7d64e8ab2812a44be70161bfa9ecfc01d69e95b8ff2a648705
ARG SOURCE_COMMIT=unknown
LABEL org.opencontainers.image.version="0.10.2" org.opencontainers.image.revision=${SOURCE_COMMIT}
COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=build /app/dist /usr/share/nginx/html
EXPOSE 8080
