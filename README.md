# HomeCloud Uninstall

Désinstalleur de HomeCloud pour une machine qui héberge un serveur. Il s'exécute dans un shell stylisé,
demande la connexion du **compte administrateur** du serveur, liste les applications HomeCloud
installées et permet de les supprimer une par une, ou toutes à la fois.

## Compilation et lancement

    make
    sudo ./homecloud-uninstall          # ou : sudo make install  puis  sudo homecloud-uninstall

Options : `--port N` (port de l'API, détecté sinon 8650), `--data DIR` (dossier de données si non détecté),
`--offline` (serveur arrêté : pas de connexion admin, root + confirmation `JE CONFIRME`), `--no-color`.

## Déroulement

1. **Connexion admin** : identifiant + mot de passe vérifiés auprès du serveur local
   (`POST /api/login`, puis `GET /api/admin/users` doit répondre 200). Un compte non administrateur est refusé.
   Si le serveur est arrêté, le programme tente de démarrer son service systemd.
2. **Liste** des composants : Serveur, Données du serveur, Données Lite, Client Qt, Shell, Web, Sync.
3. **Désinstallation** : aperçu de ce qui sera supprimé, puis confirmation.
   Les suppressions de données exigent de taper `SUPPRIMER` ; « tout supprimer » exige `TOUT SUPPRIMER`.

## Ce qui est supprimé

| Composant | Contenu |
|---|---|
| Serveur | `homecloud-server`, services systemd (arrêtés et désactivés), scripts, logs, `/opt/homecloud`… ; `/etc/homecloud` est conservé tant que des données existent |
| Données du serveur | `disks/` (.img chiffrés), `mnt/`, `meta/`, `avatars/` du dossier de données, puis `/etc/homecloud` dont les **clés LUKS (écrasées avant suppression)** |
| Données Lite | `~/.homecloud` (ou `$HC_HOME`) |
| Client / Shell / Web / Sync | exécutables, lanceurs, réglages, services |

## Sécurité

- Les volumes chiffrés montés sont démontés, les volumes LUKS fermés et les périphériques loop libérés avant suppression.
- La suppression ne suit **jamais** les liens symboliques et **ne traverse jamais** un point de montage :
  un dossier local monté en lecture seule comme disque n'est pas touché.
- Du dossier de données, seuls les sous-dossiers HomeCloud connus sont supprimés, et seulement si le dossier
  contient `disks/` ou `meta/`. Les chemins système (`/`, `/etc`, `/home`, `/srv`, un dossier personnel…) sont refusés.

## Adapter à votre installation

Les chemins, noms de services et formats de connexion sont regroupés en tête de `homecloud-uninstall.c`
(`COMPS[]`, `LOGIN_FORMATS`, `UNIT_DIRS`, `DEFAULT_PORT`, `DEFAULT_DATA`).
