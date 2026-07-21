import type { Metadata } from 'next';
import LoginForm from '@/components/LoginForm';

export const metadata: Metadata = {
  title: 'Đăng nhập · Jetsona Remote',
};

export default function LoginPage() {
  return <LoginForm />;
}
