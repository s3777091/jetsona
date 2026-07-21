import type { Metadata, Viewport } from 'next';
import { RadialGlowBackground } from '@/components/ui/background-snippets';
import './globals.css';

export const metadata: Metadata = {
  title: 'Jetsona Remote',
  description: 'Điều khiển từ xa PC và Jetson Nano qua WebRTC.',
};

export const viewport: Viewport = {
  width: 'device-width',
  initialScale: 1,
  maximumScale: 1,
  themeColor: '#05070a',
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="vi">
      <body className="antialiased">
        <RadialGlowBackground />
        {children}
      </body>
    </html>
  );
}
