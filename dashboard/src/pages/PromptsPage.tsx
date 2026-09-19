import { FileAudio2, MoreVertical, Pause, Play, Plus, Search, UploadCloud } from "lucide-react";
import { useMemo, useRef, useState } from "react";
import { api } from "../api";
import { Button, Drawer, EmptyState, ErrorState, Field, Loading, SuccessMessage, formatBytes } from "../components/ui";
import { useDateFormatter } from "../displaySettings";
import { useRemote } from "../hooks";
import type { Prompt } from "../types";

export function PromptsPage() {
  const formatDate = useDateFormatter();
  const remote = useRemote(api.prompts, []);
  const fileInput = useRef<HTMLInputElement>(null);
  const [search, setSearch] = useState("");
  const [uploadOpen, setUploadOpen] = useState(false);
  const [file, setFile] = useState<File | null>(null);
  const [name, setName] = useState("");
  const [busy, setBusy] = useState(false);
  const [playing, setPlaying] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const audioRef = useRef<HTMLAudioElement | null>(null);

  const prompts = useMemo(() => {
    const needle = search.trim().toLowerCase();
    return (remote.data ?? []).filter((prompt) => !needle || prompt.name.toLowerCase().includes(needle) || prompt.content_hash.includes(needle));
  }, [remote.data, search]);

  const chooseFile = (selected: File | undefined) => {
    if (!selected) return;
    if (selected.size > 25 * 1024 * 1024) {
      setError("Prompt files must be 25 MiB or smaller.");
      return;
    }
    setFile(selected);
    if (!name.trim()) setName(selected.name.replace(/\.[^.]+$/, ""));
    setError(null);
    setUploadOpen(true);
  };

  const upload = async () => {
    if (!file || !name.trim()) {
      setError("Choose a WAV, MP3, or OGG file and enter a name.");
      return;
    }
    setBusy(true);
    setError(null);
    try {
      const prompt = await api.uploadPrompt(name.trim(), file);
      setMessage(`${prompt.name} uploaded and normalized to 48 kHz stereo PCM16 WAV.`);
      setUploadOpen(false);
      setFile(null);
      setName("");
      await remote.refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Prompt upload failed");
    } finally {
      setBusy(false);
    }
  };

  const togglePreview = (prompt: Prompt) => {
    if (playing === prompt.id) {
      audioRef.current?.pause();
      setPlaying(null);
      return;
    }
    audioRef.current?.pause();
    const audio = new Audio(prompt.audio_url);
    audioRef.current = audio;
    audio.addEventListener("ended", () => setPlaying(null), { once: true });
    void audio.play().then(() => setPlaying(prompt.id)).catch(() => setError("The browser could not play this prompt."));
  };

  const remove = async (prompt: Prompt) => {
    if (!window.confirm(`Delete “${prompt.name}”? Draft or published usage prevents deletion.`)) return;
    setError(null);
    try {
      await api.deletePrompt(prompt.id);
      setMessage(`${prompt.name} deleted.`);
      await remote.refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Prompt could not be deleted");
    }
  };

  if (remote.loading) return <Loading label="Loading prompts" />;
  if (remote.error) return <ErrorState message={remote.error} retry={remote.refresh} />;

  return (
    <div className={`split-page ${uploadOpen ? "split-page--open" : ""}`}>
      <div className="split-page__main prompts-page">
        {message ? <SuccessMessage>{message}</SuccessMessage> : null}
        {error ? <ErrorState message={error} /> : null}
        <section
          className="upload-zone"
          onDragOver={(event) => event.preventDefault()}
          onDrop={(event) => { event.preventDefault(); chooseFile(event.dataTransfer.files[0]); }}
        >
          <input ref={fileInput} type="file" accept="audio/wav,audio/mpeg,audio/ogg,.wav,.mp3,.ogg" hidden onChange={(event) => chooseFile(event.target.files?.[0])} />
          <UploadCloud size={32} />
          <div><strong>Upload a voice prompt</strong><span>Drop WAV, MP3, or OGG here · up to 25 MiB and five minutes</span></div>
          <Button onClick={() => fileInput.current?.click()}><Plus size={18} /> Upload prompt</Button>
        </section>

        <section className="surface prompt-library">
          <div className="section-toolbar">
            <h2>Prompt library</h2>
            <label className="search-input"><Search size={19} /><input value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search prompts" /></label>
            <span>{prompts.length} prompts</span>
          </div>
          {prompts.length === 0 ? (
            <EmptyState title="No prompts uploaded">Upload the first voice prompt before adding a Play prompt step to the IVR flow.</EmptyState>
          ) : (
            <div className="table-scroll">
              <table>
                <thead><tr><th>Preview</th><th>Name</th><th>Duration</th><th>Size</th><th>Used by</th><th>Added</th><th><span className="sr-only">Actions</span></th></tr></thead>
                <tbody>{prompts.map((prompt) => (
                  <tr key={prompt.id}>
                    <td><button className="audio-button" onClick={() => togglePreview(prompt)} aria-label={`${playing === prompt.id ? "Pause" : "Play"} ${prompt.name}`}>{playing === prompt.id ? <Pause size={17} /> : <Play size={17} />}</button></td>
                    <td><span className="prompt-name"><FileAudio2 size={19} /><span><strong>{prompt.name} · v{prompt.version}</strong><small>{prompt.content_hash.slice(0, 12)}…</small></span></span></td>
                    <td>{(prompt.duration_ms / 1000).toFixed(1)} sec</td>
                    <td>{formatBytes(prompt.size_bytes)}</td>
                    <td>{prompt.used_by.join(", ") || "Not used"}</td>
                    <td>{formatDate(prompt.created_at)}</td>
                    <td><button className="icon-button" onClick={() => void remove(prompt)} aria-label={`Delete ${prompt.name}`}><MoreVertical size={19} /></button></td>
                  </tr>
                ))}</tbody>
              </table>
            </div>
          )}
        </section>
      </div>

      {uploadOpen ? (
        <Drawer title="Upload prompt" subtitle="The server validates and converts the original file." onClose={() => setUploadOpen(false)} footer={<><Button variant="secondary" onClick={() => setUploadOpen(false)}>Cancel</Button><Button onClick={() => void upload()} disabled={busy}>{busy ? "Processing…" : "Upload prompt"}</Button></>}>
          <div className="selected-file"><FileAudio2 size={27} /><span><strong>{file?.name}</strong><small>{file ? formatBytes(file.size) : ""}</small></span></div>
          <Field label="Prompt name"><input value={name} onChange={(event) => setName(event.target.value)} maxLength={120} autoFocus /></Field>
          <p className="inspector-note">Output is always validated 48 kHz, stereo, signed 16-bit PCM WAV. The uploaded source is discarded.</p>
        </Drawer>
      ) : null}
    </div>
  );
}
