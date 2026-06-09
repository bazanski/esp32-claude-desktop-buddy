# Workspace Context & Handover Rules - esp32-claude-desktop-buddy

This is the project root of **esp32-claude-desktop-buddy**.

## 🔄 State Synchronization Skill (`agentsync`)

To prevent duplicate work and ensure clean transitions across different agent sessions (e.g. Antigravity, Claude-code, Opencode), you **must** follow the **State Synchronization Pipeline** on startup and completion:

### 1. Startup Checklist (Do this FIRST before anything else)
1. **Pull the latest remote state:** Execute the following command immediately inside the project root:
   ```bash
   python3 ~/.agentsync/agentsync.py pull
   ```
2. **Read the Active State Checkpoint:** View the [.agent_state.md](.agent_state.md) file at the root of this project folder to inspect the active milestone, target goals, and troubleshooting logs.

### 2. Turn-Completion Checklist (Do this before concluding)
1. **Update local progress:** You can update the local state checkpoint manually by editing [.agent_state.md](.agent_state.md) directly, or by using the CLI save command:
   ```bash
   python3 ~/.agentsync/agentsync.py save -m "[New Milestone]" -c "[Semicolon-separated completed items]" -n "[Semicolon-separated next steps]"
   ```
2. **Push the State to GitHub:** Push your progress to the central state repo so the next local or remote agent can pull it instantly:
   ```bash
   python3 ~/.agentsync/agentsync.py sync
   ```
