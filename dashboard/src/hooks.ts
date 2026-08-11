import { useCallback, useEffect, useState } from "react";

export function useRemote<T>(loader: () => Promise<T>, dependencies: unknown[] = []) {
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);

  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      setData(await loader());
      setError(null);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Request failed");
    } finally {
      setLoading(false);
    }
  }, dependencies); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    void refresh();
  }, [refresh]);

  return { data, setData, error, loading, refresh };
}
