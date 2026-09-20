# DLSS 5 <> UEVR : Neural Reconstruction pour Unreal Engine VR

[![Platform: Windows 64-bit](https://img.shields.io/badge/Platform-Windows%20x64-blue.svg)]()
[![Graphics: DirectX 12 / DirectX 11](https://img.shields.io/badge/Graphics-DirectX%2011%20%2F%2012-brightgreen.svg)]()
[![VR: OpenXR / SteamVR](https://img.shields.io/badge/VR-OpenXR%20%7C%20SteamVR-orange.svg)]()
[![Framework: Praydog UEVR](https://img.shields.io/badge/Framework-Praydog%20UEVR-purple.svg)]()

Pont universel et suite logicielle adaptant **NVIDIA DLSS 5 Neural Reconstruction** (moteur OptiScaler Pre-SR Multipass) pour les jeux Unreal Engine 4 et 5 injectés avec **UEVR (Praydog Universal VR Mod)**, avec support complet du menu dans le casque en Réalité Virtuelle (OpenXR et OpenVR).

---

## 💡 Principes de Fonctionnement

### 1. Pourquoi le mode Pre-SR Multipass est indispensable en VR ?
- **L'approche naïve** (Post-SR) applique le modèle neuronal DLSS 5 sur l'image stéréo 4K déjà upscalée (~29.5 mégapixels pour les deux yeux). Cette passe coûte 8 à 15 ms sur GPU, ce qui détruit le budget de temps de trame VR (8.3 ms à 120 Hz, 11.1 ms à 90 Hz, 13.8 ms à 72 Hz) et provoque un décrochage immédiat en reprojection.
- **L'architecture Pre-SR Multipass** inverse le pipeline : DLSS 5 est exécuté directement sur le buffer de couleur actif à l'échelle de rendu interne (`WorkingScale = 0.75x` ou `0.66x`), traitant seulement **~4.15 mégapixels** (~0.7 à 1.6 ms sur RTX 4090/5090). L'upscaler DLSS Super Resolution prend ensuite le relais pour agrandir l'image enrichie vers la résolution cible du casque.

### 2. Préservation Intégrale du HUD en Jeu
Dans UEVR, l'interface utilisateur Unreal Engine (Slate / UMG : réticule, boussole, santé, inventaires) est interceptée dans une texture séparée (`ui_target`) puis projetée en 3D dans le casque. Comme cette composition s'effectue **après** le traitement DLSS 5, le HUD conserve une netteté vectorielle native 1:1, **sans aucun flou, ghosting ni déformation**.

### 3. Moteur Neuronal Désactivé par Défaut (Activation Explicite)
> [!IMPORTANT]
> **Le moteur neuronal DLSS 5 est volontairement désactivé par défaut (`Enabled = false`) lors de l'installation.**  
> Cela garantit que le lancement du jeu s'effectue avec 100 % des performances d'origine sans aucune charge GPU imprévue. Le joueur ouvre le menu UEVR, vérifie la télémétrie GPU, puis active le Neural Engine lorsqu'il souhaite expérimenter.

---

## 📦 Guide d'Installation Rapide

### Utilisation de l'Installateur Graphique (`VR-DLSS5-UEVR-Installer.exe`)

L'installateur autonome gère l'analyse, le déploiement et la configuration de tous les composants sans manipulation manuelle :

1. **Téléchargez et dézippez** l'archive de distribution `VR-DLSS5-UEVR-Release.zip`.
2. **Lancez `VR-DLSS5-UEVR-Installer.exe`**.
3. **Sélectionnez l'exécutable du jeu** :
   - Cliquez sur **Browse...** ou glissez-déposez le fichier `.exe` du jeu dans la fenêtre.
   - *Détection intelligente* : Si vous sélectionnez le launcher à la racine du jeu, l'installateur détecte automatiquement le véritable exécutable Unreal Engine dans `Binaries\Win64`.
4. **Sélectionnez votre runtime NVIDIA DLSS 5 (`nvngx_dlssnr.dll`)** :
   - Cliquez sur **Select file...** et pointez vers votre fichier `nvngx_dlssnr.dll` (build **310.8**).
   - *Vérification SHA-256 automatique* : L'installateur valide l'empreinte cryptographique du fichier :
     - `E16BCF...` : Modèle officiel signé NVIDIA (RTX 50).
     - `E67DEE...` : Modèle cross-génération ShortFuse (RTX 20, 30 et 40).
   - Le chemin du fichier est mémorisé pour les futures installations.
5. **Cliquez sur `Install / Update DLSS 5`** :
   - L'installateur vérifie si le jeu est en cours d'exécution et propose de le fermer proprement pour déverrouiller les fichiers.
   - Il nettoie automatiquement les anciens mods conflictuels (ReShade, RenoDX).
   - Il déploie :
     - `OptiScaler.dll` (moteur Pre-SR Multipass avec canal IPC temps réel).
     - `OptiScaler.ini` préconfiguré pour la VR (FrameGen désactivé, logs disque désactivés, Pre-SR actif, DLSS 5 en attente d'activation).
     - `VRDLSS5_UEVR_Plugin.dll` dans `<Jeu>\Binaries\Win64\uevr\plugins\` ainsi que dans `%APPDATA%\UnrealVRMod\<Jeu>\plugins\`.
     - `VRDLSS5.lua` dans `<Jeu>\Binaries\Win64\uevr\scripts\` ainsi que dans `%APPDATA%\UnrealVRMod\<Jeu>\scripts\`.
     - `nvngx.dll_dlssnr.dll`, `cudart64_12.dll` et `nvngx_dlssnr.dll`.
6. **Lancez le jeu**, puis injectez UEVR avec `UEVRInjector.exe` comme d'habitude.

---

## 🎮 Contrôles & Menu en Jeu

L'interface de contrôle est intégrée **directement et nativement dans UEVR** :

| Action | Raccourci Manette / VR | Raccourci Clavier |
| :--- | :--- | :--- |
| **Ouvrir le menu UEVR** | **`L3 + R3`** (Clic simultané des deux sticks) | **`Insert`** |
| **Accéder aux réglages DLSS 5** | Onglet **`DLSS 5 Neural Reconstruction`** dans le volet UEVR | Même onglet |
| **Ouvrir l'overlay autonome (Direct)** | - | **`F6`** ou **`Home`** |
| **Fermer l'overlay autonome** | - | **`Escape`** ou **`F6`** |
| **Bascule rapide Ray Reconstruction** | - | **`F8`** |

> [!NOTE]
> Aucun hook manette custom n'est actif en tâche de fond. Le plugin s'appuie à 100 % sur l'interception native de UEVR (`L3 + R3`), garantissant **0 % de surcharge CPU** sur le thread de rendu.

---

## ⚙️ Paramètres Disponibles dans le Menu

| Section | Paramètre | Valeur par défaut | Description & Recommandation |
| :--- | :--- | :--- | :--- |
| **En-tête** | **Enable Neural Engine** | `Désactivé` | Active ou désactive le traitement neuronal DLSS 5 en temps réel. |
| **Général** | **VR WorkingScale** | `0.75x` | Échelle d'inférence neuronale. Réduire à `0.66x` ou `0.50x` sur les scènes lourdes ou les casques à haute fréquence (90/120 Hz). |
| **Général** | **Run Before SR** | `Activé` | **Indispensable en VR.** Applique DLSS 5 avant l'upscaling pour diviser la surface de calcul par 4. |
| **Général** | **ResidualAcrossRR** | `Désactivé` | Préservation du résidu avec Ray Reconstruction. Laisser désactivé sauf si le jeu utilise activement le Ray Reconstruction. |
| **Général** | **AI Model Preset** | `2 - Performance` | Préréglage du réseau. Le mode 2 (Performance) offre le meilleur ratio netteté / temps de calcul en VR. |
| **Général** | **DLSS 5 Style** | `0 - Standard` | Style de rendu (0: Standard, 1: Naturel, 2: Cinématique). |
| **Général** | **DLSS 5 Intensity** | `1.00x` | Intensité de la reconstruction des micro-détails (0.00x à 2.00x). |
| **Avancé** | **Structure Décor** | `1.00x` | Accentuation des surfaces et éléments géométriques de l'environnement. |
| **Avancé** | **Tonalité Ombres** | `0.00x` | Équilibrage des zones d'ombres et des reflets spéculaires. |
| **Avancé** | **Masque Auto** | `Activé` | Masquage sémantique automatique des personnages pour éviter toute distorsion faciale. |
| **Avancé** | **Structure Peau** | `-1.00x` | Adoucissement et préservation des textures de peau (-1.00 = automatique). |
| **Avancé** | **Passes IA** | `1 - Simple Passe`| Nombre de passes en chaîne. Laisser sur 1 en VR pour préserver le framerate. |
| **Sécurité** | **Dynamic VR Frame Guard** | `Activé` | Surveille le temps de trame GPU en continu. Si le budget VR est menacé (>13 ms à 72 Hz), l'échelle est automatiquement réduite pour éviter toute chute de FPS. |

---

## 🔄 Désinstallation & Restauration

Pour retirer DLSS 5 d'un jeu :
1. Relancez **`VR-DLSS5-UEVR-Installer.exe`**.
2. Sélectionnez l'exécutable du jeu.
3. Cliquez sur **`Restore Original`**.  
   L'installateur supprime les DLLs injectées (`OptiScaler.dll`, `VRDLSS5_UEVR_Plugin.dll`, `VRDLSS5.lua`, etc.) et restaure l'état d'origine du jeu.

---

## 🛠️ Compilation Depuis les Sources

### Prérequis
- Windows 10/11 x64
- Visual Studio 2022 avec les composants C++ (MSVC `cl.exe` v143+ avec support C++20).

### 1. Build Global en Un Clic
```cmd
cd C:\code\vrdlss5-uevr
package_release.bat
```
Ce script compile l'ensemble des modules et génère le dossier ainsi que l'archive :
- `dist/VR-DLSS5-UEVR-Release/`
- `dist/VR-DLSS5-UEVR-Release.zip`

### 2. Compilation Individuelle des Modules
- **Plugin UEVR** :
  ```cmd
  cd C:\code\vrdlss5-uevr\plugin
  build.bat
  ```
- **Installateur Graphique** :
  ```cmd
  cd C:\code\vrdlss5-uevr\installer
  build.bat
  ```
- **Dual-Proxy Standalone** :
  ```cmd
  cd C:\code\vrdlss5-uevr\proxy
  build.bat
  ```

---

## 📂 Organisation du Dépôt

```
vrdlss5-uevr/
├── installer/             # Installateur Win32 GUI autonome (installer.cpp, build.bat)
├── plugin/                # Plugin C++ UEVR (VRDLSS5_Plugin.cpp, build.bat)
├── scripts/               # Script Lua pour l'interface native UEVR (VRDLSS5.lua)
├── proxy/                 # Dual-proxy optionnel (DXGI + OpenVR Overlay)
├── deps/                  # Moteur OptiScaler Pre-SR et OptiScaler.ini configuré VR
├── dist/                  # Packages de release prêts pour distribution
├── doc/                   # Documentation technique détaillée d'architecture
├── package_release.bat    # Script de build et packaging automatisé
└── README.md              # Documentation principale
```

---

## ⚖️ Mentions Légales
- Ce projet est un mod indépendant et n'est pas affilié à NVIDIA, Epic Games ou Valve.
- Le runtime NVIDIA Neural Rendering (`nvngx_dlssnr.dll`) est la propriété exclusive de NVIDIA Corporation et n'est pas redistribué par ce projet. L'utilisateur doit se le procurer par ses propres moyens selon les termes de NVIDIA.
- UEVR est développé par praydog sous licence MIT.
