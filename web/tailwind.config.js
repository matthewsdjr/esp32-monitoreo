/** @type {import('tailwindcss').Config} */
export default {
  content: ["./index.html", "./src/**/*.{js,ts,jsx,tsx}"],
  // El toggle del usuario (data-theme) debe poder ganarle a la preferencia del
  // sistema en ambos sentidos, no solo forzar oscuro.
  darkMode: ["variant", [
    '&:where([data-theme="dark"], [data-theme="dark"] *)',
    '@media (prefers-color-scheme: dark) { &:where(:not([data-theme="light"] *)) }',
  ]],
  theme: {
    extend: {
      fontFamily: {
        sans: ["system-ui", "-apple-system", '"Segoe UI"', "sans-serif"],
      },
    },
  },
  plugins: [],
};
