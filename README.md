# Claudeputer

Discuter avec **Claude** depuis un **M5Stack Cardputer**, en WiFi. ⌨️🤖

Le Cardputer se connecte au WiFi, tu tapes ta question au clavier, elle est
envoyée à l'API Messages d'Anthropic, et la réponse s'affiche à l'écran
(scrollable). Première version volontairement **simple** — l'interface sera
retravaillée ensuite.

> ⚠️ **Sécurité du compte** : ta clé API et ton mot de passe WiFi vivent dans
> `src/config.h`, qui est **git-ignoré**. Ne les commit jamais. La requête part
> directement du Cardputer vers `api.anthropic.com`.

---

## Matériel supporté

| Appareil            | Statut        | Note                                              |
|---------------------|---------------|---------------------------------------------------|
| Cardputer **1.1**   | ✅ cible principale | ESP32-S3 (StampS3), librairie `M5Cardputer` mature |
| Cardputer **ADV**   | 🧪 expérimental    | Même famille ESP32-S3 ; env `cardputer_adv`        |

---

## Démarrage rapide

### 1. Récupérer le code
```bash
git clone <url-du-repo>
cd claudeputer
```

### 2. Configurer tes secrets
```bash
cp src/config.h.example src/config.h
```
Édite `src/config.h` :
- `WIFI_SSID` / `WIFI_PASSWORD`
- `ANTHROPIC_API_KEY` — depuis https://console.anthropic.com/settings/keys
- `CLAUDE_MODEL` — par défaut `claude-haiku-4-5` (rapide & pas cher, idéal petit écran)

### 3a. Compiler & flasher avec PlatformIO (recommandé)
Avec [PlatformIO](https://platformio.org/) (extension VS Code) :
```bash
pio run -e cardputer -t upload      # Cardputer 1.1
pio device monitor                  # logs série
```
Pour l'ADV : `pio run -e cardputer_adv -t upload`

### 3b. Ou avec l'Arduino IDE
1. Installe le support **ESP32** (board manager → "esp32" par Espressif).
2. Carte : **M5Stack StampS3** (ou "M5Cardputer" si proposée), PSRAM activée.
3. Bibliothèques (Library Manager) : **M5Cardputer** et **ArduinoJson** (v7).
4. Copie `src/main.cpp` dans un sketch `.ino`, ajoute `config.h` à côté, et téléverse.

---

## Utilisation

| Écran        | Touche        | Action                                  |
|--------------|---------------|-----------------------------------------|
| Saisie       | *(taper)*     | Écrire la question                      |
| Saisie       | `ENTER`       | Envoyer à Claude                        |
| Saisie       | `DEL`         | Effacer un caractère                    |
| Réponse      | `;` / `.`     | Scroller haut / bas                     |
| Réponse      | `espace`      | Page suivante                           |
| Réponse      | `` ` ``       | Revenir à la saisie (nouvelle question) |

L'historique de conversation est conservé (`MAX_HISTORY_TURNS` tours) pour que
Claude garde le contexte.

---

## Comment ça marche

```
Cardputer ──WiFi──> HTTPS POST api.anthropic.com/v1/messages
   │                     headers: x-api-key, anthropic-version
   │                     body: { model, system, messages[] }
   └──< réponse JSON ─── content[].text  ──> affichage scrollable
```

- TLS via `WiFiClientSecure` (v1 : `setInsecure()`, validation de certificat à
  durcir plus tard).
- JSON via **ArduinoJson v7**.

---

## Limitations connues (v1) & idées suivantes

- [ ] Validation du certificat TLS (root CA épinglé) au lieu de `setInsecure()`
- [ ] Réponses en **streaming** (SSE) plutôt qu'en bloc
- [ ] Saisie multi-lignes + édition du curseur
- [ ] Indicateur d'usage de tokens / coût
- [ ] Sauvegarde des conversations sur carte SD
- [ ] Réglages à l'écran (modèle, max_tokens) sans recompiler
- [ ] Profil dédié et test du **Cardputer ADV**

---

## Licence

MIT — voir [LICENSE](LICENSE).
