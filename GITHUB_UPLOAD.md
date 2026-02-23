# M5StamPLC auf GitHub hochladen

## 1. Neues Repository auf GitHub anlegen

1. Öffne [github.com/new](https://github.com/new).
2. **Repository name:** `M5StamPLC` (oder ein anderer Name).
3. **Description:** z. B. „M5StampPLC RFID-Controller mit RS485, INA226, Relais“.
4. **Public** wählen, **ohne** „Add a README“ (wir haben schon eine).
5. Auf **Create repository** klicken.

## 2. Lokales Git-Repo und erster Push

In **PowerShell** oder **Git Bash** im Projektordner ausführen:

```powershell
cd C:\Users\benja\M5StamPLC

git init
git add .
git commit -m "Initial commit: M5StampPLC RFID-Controller Sketch"

# Ersetze DEIN-USERNAME durch deinen GitHub-Benutzernamen:
git remote add origin https://github.com/DEIN-USERNAME/M5StamPLC.git

git branch -M main
git push -u origin main
```

Falls du SSH nutzt:

```powershell
git remote add origin git@github.com:DEIN-USERNAME/M5StamPLC.git
git push -u origin main
```

## 3. Bei Login-Abfrage

- Bei **HTTPS** werden Benutzername und Passwort bzw. **Personal Access Token** abgefragt.
- Token anlegen: GitHub → Settings → Developer settings → Personal access tokens → Generate new token (z. B. mit Scope `repo`).

Danach ist das Projekt unter `https://github.com/DEIN-USERNAME/M5StamPLC` sichtbar.
