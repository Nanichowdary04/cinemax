import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import App from './App';
import './styles.css';

// Desktop-app feel: no browser context menu / zoom / text selection drags.
window.addEventListener('contextmenu', (e) => {
  const t = e.target as HTMLElement | null;
  if (!t || !t.closest('input, textarea')) e.preventDefault();
});
window.addEventListener(
  'wheel',
  (e) => {
    if (e.ctrlKey) e.preventDefault();
  },
  { passive: false },
);
window.addEventListener('keydown', (e) => {
  if (e.ctrlKey && ['+', '-', '=', '0'].includes(e.key)) e.preventDefault();
});

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
