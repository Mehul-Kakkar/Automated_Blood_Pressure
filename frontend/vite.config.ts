import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { VitePWA } from 'vite-plugin-pwa'

export default defineConfig({
  plugins: [react(), VitePWA({ registerType: 'prompt', includeAssets: [], manifest: { name: 'Device Companion', short_name: 'Companion', description: 'Companion for an experimental monitoring device', theme_color: '#f7f6f3', background_color: '#f7f6f3', display: 'standalone', start_url: '/', icons: [{ src: '/icon.svg', sizes: 'any', type: 'image/svg+xml', purpose: 'any' }] }, workbox: { globPatterns: ['**/*.{js,css,html,svg,woff2}'], navigateFallback: '/index.html' } })],
})
