import { useEffect, useRef, type ButtonHTMLAttributes, type PropsWithChildren, type ReactNode } from "react";
import { AlertCircle, CheckCircle2, LoaderCircle, X } from "lucide-react";

export function Button({
  variant = "primary",
  className = "",
  ...props
}: ButtonHTMLAttributes<HTMLButtonElement> & {
  variant?: "primary" | "secondary" | "danger" | "ghost";
}) {
  return <button className={`button button--${variant} ${className}`} {...props} />;
}

export function Loading({ label = "Loading" }: { label?: string }) {
  return (
    <div className="loading" role="status">
      <LoaderCircle size={20} className="spin" />
      <span>{label}</span>
    </div>
  );
}

export function ErrorState({ message, retry }: { message: string; retry?: () => void }) {
  return (
    <div className="message message--error" role="alert">
      <AlertCircle size={20} />
      <span>{message}</span>
      {retry ? (
        <Button variant="secondary" onClick={retry}>
          Retry
        </Button>
      ) : null}
    </div>
  );
}

export function SuccessMessage({ children }: PropsWithChildren) {
  return (
    <div className="message message--success" role="status">
      <CheckCircle2 size={20} />
      <span>{children}</span>
    </div>
  );
}

export function EmptyState({ title, children }: PropsWithChildren<{ title: string }>) {
  return (
    <div className="empty-state">
      <h3>{title}</h3>
      <p>{children}</p>
    </div>
  );
}

export function Drawer({
  title,
  subtitle,
  onClose,
  children,
  footer,
}: PropsWithChildren<{
  title: string;
  subtitle?: string;
  onClose: () => void;
  footer?: ReactNode;
}>) {
  const closeButton = useRef<HTMLButtonElement>(null);
  const closeHandler = useRef(onClose);
  closeHandler.current = onClose;
  useEffect(() => {
    const previous = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    const escape = (event: KeyboardEvent) => {
      if (event.key === "Escape") closeHandler.current();
    };
    document.addEventListener("keydown", escape);
    closeButton.current?.focus();
    return () => {
      document.removeEventListener("keydown", escape);
      previous?.focus();
    };
  }, []);
  return (
    <aside className="drawer" role="dialog" aria-modal="true" aria-label={title}>
      <header className="drawer__header">
        <div>
          <h2>{title}</h2>
          {subtitle ? <p>{subtitle}</p> : null}
        </div>
        <button ref={closeButton} className="icon-button" onClick={onClose} aria-label="Close panel">
          <X size={22} />
        </button>
      </header>
      <div className="drawer__body">{children}</div>
      {footer ? <footer className="drawer__footer">{footer}</footer> : null}
    </aside>
  );
}

export function Modal({
  title,
  children,
  onClose,
  wide = false,
}: PropsWithChildren<{ title: string; onClose: () => void; wide?: boolean }>) {
  const closeButton = useRef<HTMLButtonElement>(null);
  const closeHandler = useRef(onClose);
  closeHandler.current = onClose;
  useEffect(() => {
    const previous = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    const escape = (event: KeyboardEvent) => {
      if (event.key === "Escape") closeHandler.current();
    };
    document.addEventListener("keydown", escape);
    closeButton.current?.focus();
    return () => {
      document.removeEventListener("keydown", escape);
      previous?.focus();
    };
  }, []);
  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={onClose}>
      <section
        className={`modal ${wide ? "modal--wide" : ""}`}
        role="dialog"
        aria-modal="true"
        aria-label={title}
        onMouseDown={(event) => event.stopPropagation()}
      >
        <header className="modal__header">
          <h2>{title}</h2>
          <button ref={closeButton} type="button" onClick={onClose} aria-label={`Close ${title}`}>
            <X size={19} />
          </button>
        </header>
        {children}
      </section>
    </div>
  );
}

export function Field({
  label,
  hint,
  children,
}: PropsWithChildren<{ label: string; hint?: string }>) {
  return (
    <label className="field">
      <span className="field__label">{label}</span>
      {children}
      {hint ? <span className="field__hint">{hint}</span> : null}
    </label>
  );
}

export function formatBytes(value: number): string {
  if (value < 1024) return `${value} B`;
  if (value < 1024 ** 2) return `${(value / 1024).toFixed(1)} KB`;
  if (value < 1024 ** 3) return `${(value / 1024 ** 2).toFixed(1)} MB`;
  return `${(value / 1024 ** 3).toFixed(1)} GB`;
}

export function formatDuration(seconds: number): string {
  const minutes = Math.floor(seconds / 60);
  const remaining = Math.max(0, seconds % 60);
  return `${String(minutes).padStart(2, "0")}:${String(remaining).padStart(2, "0")}`;
}
